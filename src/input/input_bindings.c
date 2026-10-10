/*
 * input_bindings.c -- read the binding config, and sample the four ports.
 *
 * Structure:
 *   CONTROLS[]   the 24 Xbox inputs, by the names the config file uses
 *   DEFAULTS[]   what each control reads with no config file: one pad source
 *                and, on port 1, one keyboard source
 *   a small JSON reader (no dependency is worth one file's worth of braces)
 *   resolve      binding strings -> Source records, once, at load
 *   sample       Source records -> an XBOX_GAMEPAD, every poll
 *
 * Sources are resolved to key codes and pad indices at load time, so a poll
 * does no string work. A poll reads each distinct key once (KeyCache) and
 * each pad once, because a title polls its pads several times a frame.
 *
 * The mouse: its buttons are sources ("mouse:left"), and the top-level
 * "mouse" object configures mouse look for a title that has a hook for it
 * (recomp_mouse.h).
 */
#include "input_bindings.h"
#include "recomp_mouse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <xinput.h>
#endif

#define MAX_SOURCES 4
#define PORTS XBOX_MAX_CONTROLLERS

/* XInput's own left-stick default. A stick is bound as two halves, so the
 * deadzone is applied per half rather than radially: a title that wants a
 * circular one applies its own on top, as it did on the console. */
#define DEADZONE_DEFAULT 7849

/* ---- the controls a title can read ------------------------------------- */

enum { K_DIGITAL, K_ANALOG, K_AXIS };

/* XINPUT_GAMEPAD_TRIGGER_THRESHOLD: the value XInput documents as the point
 * a trigger counts as pressed. */
#define TRIGGER_THRESHOLD 30
enum { AXIS_LX, AXIS_LY, AXIS_RX, AXIS_RY };

static const struct {
    const char *name;
    unsigned char kind;
    unsigned short arg;        /* digital: button bit; analog: index; axis: id */
    signed char sign;          /* axis only: which half of the axis */
} CONTROLS[] = {
    { "a",            K_ANALOG,  XBOX_BUTTON_A,            0 },
    { "b",            K_ANALOG,  XBOX_BUTTON_B,            0 },
    { "x",            K_ANALOG,  XBOX_BUTTON_X,            0 },
    { "y",            K_ANALOG,  XBOX_BUTTON_Y,            0 },
    { "black",        K_ANALOG,  XBOX_BUTTON_BLACK,        0 },
    { "white",        K_ANALOG,  XBOX_BUTTON_WHITE,        0 },
    { "start",        K_DIGITAL, XBOX_GAMEPAD_START,       0 },
    { "back",         K_DIGITAL, XBOX_GAMEPAD_BACK,        0 },
    { "ltrigger",     K_ANALOG,  XBOX_BUTTON_LTRIGGER,     0 },
    { "rtrigger",     K_ANALOG,  XBOX_BUTTON_RTRIGGER,     0 },
    { "dpad_up",      K_DIGITAL, XBOX_GAMEPAD_DPAD_UP,     0 },
    { "dpad_down",    K_DIGITAL, XBOX_GAMEPAD_DPAD_DOWN,   0 },
    { "dpad_left",    K_DIGITAL, XBOX_GAMEPAD_DPAD_LEFT,   0 },
    { "dpad_right",   K_DIGITAL, XBOX_GAMEPAD_DPAD_RIGHT,  0 },
    { "lstick_up",    K_AXIS,    AXIS_LY,                  1 },
    { "lstick_down",  K_AXIS,    AXIS_LY,                 -1 },
    { "lstick_left",  K_AXIS,    AXIS_LX,                 -1 },
    { "lstick_right", K_AXIS,    AXIS_LX,                  1 },
    { "lthumb",       K_DIGITAL, XBOX_GAMEPAD_LEFT_THUMB,  0 },
    { "rstick_up",    K_AXIS,    AXIS_RY,                  1 },
    { "rstick_down",  K_AXIS,    AXIS_RY,                 -1 },
    { "rstick_left",  K_AXIS,    AXIS_RX,                 -1 },
    { "rstick_right", K_AXIS,    AXIS_RX,                  1 },
    { "rthumb",       K_DIGITAL, XBOX_GAMEPAD_RIGHT_THUMB, 0 },
};
#define CONTROL_COUNT ((int)(sizeof CONTROLS / sizeof CONTROLS[0]))

/* The built-in mapping, and the one the UI offers as "reset to defaults".
 * The keyboard column is what this runtime has always used: arrows = D-pad,
 * Enter = START, Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White,
 * W = Black, E/R = triggers. It is applied to port 1 only. Sticks have no
 * keyboard default: every comfortable key is already a button here.
 *
 * tools/input_ui/test_runtime_vocabulary.py reads this table out of this file
 * and fails if the UI's defaults have drifted from it. */
static const struct { const char *control, *pad, *key; } DEFAULTS[] = {
    { "a",            "pad:a",          "key:Z"      },
    { "b",            "pad:b",          "key:X"      },
    { "x",            "pad:x",          "key:A"      },
    { "y",            "pad:y",          "key:S"      },
    { "black",        "pad:lshoulder",  "key:W"      },
    { "white",        "pad:rshoulder",  "key:Q"      },
    { "ltrigger",     "pad:lt",         "key:E"      },
    { "rtrigger",     "pad:rt",         "key:R"      },
    { "start",        "pad:start",      "key:RETURN" },
    { "back",         "pad:back",       "key:BACK"   },
    { "dpad_up",      "pad:dpad_up",    "key:UP"     },
    { "dpad_down",    "pad:dpad_down",  "key:DOWN"   },
    { "dpad_left",    "pad:dpad_left",  "key:LEFT"   },
    { "dpad_right",   "pad:dpad_right", "key:RIGHT"  },
    { "lstick_up",    "pad:ly+",        NULL         },
    { "lstick_down",  "pad:ly-",        NULL         },
    { "lstick_left",  "pad:lx-",        NULL         },
    { "lstick_right", "pad:lx+",        NULL         },
    { "lthumb",       "pad:lthumb",     NULL         },
    { "rstick_up",    "pad:ry+",        NULL         },
    { "rstick_down",  "pad:ry-",        NULL         },
    { "rstick_left",  "pad:rx-",        NULL         },
    { "rstick_right", "pad:rx+",        NULL         },
    { "rthumb",       "pad:rthumb",     NULL         },
};

/* ---- the sources a control can be bound to ----------------------------- */

enum { SRC_NONE, SRC_KEY, SRC_PAD_BUTTON, SRC_PAD_TRIGGER, SRC_PAD_AXIS, SRC_MOUSE };

/* What a "mouse:" source may name, in RECOMP_MOUSE_LEFT.. order. A wheel
 * notch is a short press (recomp_mouse_source), so the wheel binds to a
 * button the way it does in a PC game: next weapon, previous weapon. */
static const char *const MOUSE_SOURCES[RECOMP_MOUSE_SOURCE_COUNT] = {
    "left", "right", "middle", "x1", "x2", "wheel_up", "wheel_down"
};

/* Pad buttons, in the order PAD_BUTTONS names them. Kept as an index rather
 * than an XInput mask so this file parses a config on a host with no
 * <xinput.h>. */
enum {
    PB_A, PB_B, PB_X, PB_Y, PB_LSHOULDER, PB_RSHOULDER, PB_START, PB_BACK,
    PB_LTHUMB, PB_RTHUMB, PB_DPAD_UP, PB_DPAD_DOWN, PB_DPAD_LEFT, PB_DPAD_RIGHT,
    PB_COUNT
};

static const char *const PAD_BUTTONS[PB_COUNT] = {
    "a", "b", "x", "y", "lshoulder", "rshoulder", "start", "back",
    "lthumb", "rthumb", "dpad_up", "dpad_down", "dpad_left", "dpad_right"
};

typedef struct {
    unsigned char kind;
    short arg;              /* key: virtual-key code; pad: button/axis index */
    signed char sign;       /* axis only */
} Source;

typedef struct {
    unsigned char count;
    Source src[MAX_SOURCES];
} Binding;

enum { DEV_NONE, DEV_KEYBOARD, DEV_XINPUT };

typedef struct {
    int device;
    int pad;                /* XInput index when device == DEV_XINPUT */
    int deadzone;
    int has_key;            /* any key: source, so the port works with no pad */
    int has_mouse;          /* any mouse: source */
    Binding bind[CONTROL_COUNT];
    char label[48];
} Controller;

static Controller g_ctl[PORTS];
static char g_path[520];
static int g_have_path;

/* Mouse look settings: the file's top-level "mouse", then the RECOMP_MOUSE*
 * variables over it. Handed to recomp_mouse.c at load. */
static RecompMouseConfig g_mouse;

/* ---- names -> codes ---------------------------------------------------- */

/* Virtual-key names the config file may use. Single letters and digits are
 * their own code on Windows and are handled without a table entry; "key:#13"
 * passes a raw decimal code through for anything named nowhere. */
static const struct { const char *name; int vk; } KEYS[] = {
    { "UP", 0x26 }, { "DOWN", 0x28 }, { "LEFT", 0x25 }, { "RIGHT", 0x27 },
    { "RETURN", 0x0D }, { "ENTER", 0x0D }, { "BACK", 0x08 },
    { "BACKSPACE", 0x08 }, { "SPACE", 0x20 }, { "TAB", 0x09 },
    { "ESCAPE", 0x1B }, { "SHIFT", 0x10 }, { "LSHIFT", 0xA0 },
    { "RSHIFT", 0xA1 }, { "CTRL", 0x11 }, { "LCTRL", 0xA2 }, { "RCTRL", 0xA3 },
    { "ALT", 0x12 }, { "LALT", 0xA4 }, { "RALT", 0xA5 },
    { "CAPSLOCK", 0x14 }, { "INSERT", 0x2D }, { "DELETE", 0x2E },
    { "HOME", 0x24 }, { "END", 0x23 }, { "PAGEUP", 0x21 }, { "PAGEDOWN", 0x22 },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 },
    { "F5", 0x74 }, { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 },
    { "F9", 0x78 }, { "F10", 0x79 }, { "F11", 0x7A }, { "F12", 0x7B },
    { "NUMPAD0", 0x60 }, { "NUMPAD1", 0x61 }, { "NUMPAD2", 0x62 },
    { "NUMPAD3", 0x63 }, { "NUMPAD4", 0x64 }, { "NUMPAD5", 0x65 },
    { "NUMPAD6", 0x66 }, { "NUMPAD7", 0x67 }, { "NUMPAD8", 0x68 },
    { "NUMPAD9", 0x69 }, { "MULTIPLY", 0x6A }, { "ADD", 0x6B },
    { "SUBTRACT", 0x6D }, { "DECIMAL", 0x6E }, { "DIVIDE", 0x6F },
    { "SEMICOLON", 0xBA }, { "EQUALS", 0xBB }, { "COMMA", 0xBC },
    { "MINUS", 0xBD }, { "PERIOD", 0xBE }, { "SLASH", 0xBF },
    { "GRAVE", 0xC0 }, { "LBRACKET", 0xDB }, { "BACKSLASH", 0xDC },
    { "RBRACKET", 0xDD }, { "QUOTE", 0xDE },
};

static int control_index(const char *name)
{
    int i;
    for (i = 0; i < CONTROL_COUNT; i++)
        if (strcmp(CONTROLS[i].name, name) == 0)
            return i;
    return -1;
}

static int key_code(const char *name)
{
    size_t i;

    if (name[0] == '#')
        return (int)strtol(name + 1, NULL, 10);
    if (name[1] == '\0') {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (int)(unsigned char)c;
        return 0;
    }
    for (i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
        if (strcmp(KEYS[i].name, name) == 0)
            return KEYS[i].vk;
    return 0;
}

/* "pad:lx-", "key:RETURN", "none" -> a Source. Returns 0 on a name this
 * build does not know, which is reported once rather than per control. */
static int parse_source(const char *text, Source *out)
{
    memset(out, 0, sizeof *out);
    if (!text || !*text || strcmp(text, "none") == 0)
        return 1;                                     /* deliberately unbound */
    if (strncmp(text, "key:", 4) == 0) {
        int vk = key_code(text + 4);
        if (!vk)
            return 0;
        out->kind = SRC_KEY;
        out->arg = (short)vk;
        return 1;
    }
    if (strncmp(text, "pad:", 4) == 0) {
        const char *name = text + 4;
        int i;

        if (strcmp(name, "lt") == 0 || strcmp(name, "rt") == 0) {
            out->kind = SRC_PAD_TRIGGER;
            out->arg = (short)(name[0] == 'r');
            return 1;
        }
        for (i = 0; i < PB_COUNT; i++)
            if (strcmp(PAD_BUTTONS[i], name) == 0) {
                out->kind = SRC_PAD_BUTTON;
                out->arg = (short)i;
                return 1;
            }
        /* half axes: lx+ ly- rx+ ry- */
        if (strlen(name) == 3 && (name[2] == '+' || name[2] == '-') &&
            (name[0] == 'l' || name[0] == 'r') &&
            (name[1] == 'x' || name[1] == 'y')) {
            out->kind = SRC_PAD_AXIS;
            out->arg = (short)((name[0] == 'r' ? 2 : 0) + (name[1] == 'y'));
            out->sign = (signed char)(name[2] == '+' ? 1 : -1);
            return 1;
        }
    }
    if (strncmp(text, "mouse:", 6) == 0) {
        int i;

        for (i = 0; i < RECOMP_MOUSE_SOURCE_COUNT; i++)
            if (strcmp(MOUSE_SOURCES[i], text + 6) == 0) {
                out->kind = SRC_MOUSE;
                out->arg = (short)i;
                return 1;
            }
    }
    return 0;
}

static void bind_add(Binding *b, const char *text, int *bad)
{
    Source s;

    if (b->count >= MAX_SOURCES)
        return;
    if (!parse_source(text, &s)) {
        (*bad)++;
        return;
    }
    if (s.kind == SRC_NONE)
        return;
    b->src[b->count++] = s;
}

/* ---- defaults ---------------------------------------------------------- */

static void defaults_for(Controller *c, int port)
{
    int i, bad = 0;

    memset(c, 0, sizeof *c);
    c->device = DEV_XINPUT;
    c->pad = port;
    c->deadzone = DEADZONE_DEFAULT;
    for (i = 0; i < (int)(sizeof DEFAULTS / sizeof DEFAULTS[0]); i++) {
        int k = control_index(DEFAULTS[i].control);
        if (k < 0)
            continue;
        bind_add(&c->bind[k], DEFAULTS[i].pad, &bad);
        if (port == 0 && DEFAULTS[i].key)
            bind_add(&c->bind[k], DEFAULTS[i].key, &bad);
    }
    c->has_key = (port == 0);
}

/* ---- a small JSON reader ----------------------------------------------- */

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* p is at the opening quote. Copies at most n-1 characters; the escapes a
 * config file can contain are handled and \u is left as written. */
static const char *jstring(const char *p, char *out, size_t n)
{
    size_t i = 0;

    if (*p != '"')
        return NULL;
    p++;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default:  c = e;    break;
            }
        }
        if (i + 1 < n)
            out[i++] = c;
    }
    if (*p != '"')
        return NULL;
    out[i] = '\0';
    return p + 1;
}

static const char *jskip(const char *p)
{
    p = ws(p);
    if (*p == '"') {
        char discard[8];
        const char *q = p;
        /* jstring with a tiny buffer still walks the whole string. */
        return jstring(q, discard, sizeof discard);
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (char)(open == '{' ? '}' : ']');
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char discard[8];
                const char *q = jstring(p, discard, sizeof discard);
                if (!q)
                    return NULL;
                p = q;
                continue;
            }
            if (*p == open)
                depth++;
            else if (*p == close && --depth == 0)
                return p + 1;
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']')
        p++;
    return p;
}

/* { "a": "pad:a", "b": ["pad:b", "key:X"] } */
static const char *parse_bindings(const char *p, Controller *c, int *bad)
{
    char key[48], value[64];

    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    if (*p == '}')
        return p + 1;
    for (;;) {
        int k;

        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        k = control_index(key);
        if (k >= 0)
            c->bind[k].count = 0;      /* a named control replaces its default */
        if (*p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = jstring(p, value, sizeof value);
                if (!p)
                    return NULL;
                if (k >= 0)
                    bind_add(&c->bind[k], value, bad);
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return NULL;
            p++;
        } else if (*p == '"') {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            if (k >= 0)
                bind_add(&c->bind[k], value, bad);
        } else {
            p = jskip(p);                        /* null, or something odd */
            if (!p)
                return NULL;
        }
        if (k < 0)
            (*bad)++;
        p = ws(p);
        if (*p == ',') {
            p = ws(p + 1);
            continue;
        }
        if (*p != '}')
            return NULL;
        return p + 1;
    }
}

static void set_device(Controller *c, const char *name)
{
    if (strcmp(name, "keyboard") == 0) {
        c->device = DEV_KEYBOARD;
        c->pad = -1;
    } else if (strcmp(name, "none") == 0 || !*name) {
        c->device = DEV_NONE;
        c->pad = -1;
    } else if (strncmp(name, "xinput:", 7) == 0) {
        int n = (int)strtol(name + 7, NULL, 10);
        c->device = DEV_XINPUT;
        c->pad = (n >= 0 && n < PORTS) ? n : 0;
    }
}

/* One element of "controllers". Starts from the defaults for whichever port
 * it names, so a file that lists two bindings is a two-binding change. */
static const char *parse_controller(const char *p, int index, int *bad)
{
    Controller tmp;
    char key[48], value[64];
    int port = index, seen_port = 0, i;

    defaults_for(&tmp, index < PORTS ? index : 0);
    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (strcmp(key, "port") == 0) {
            port = (int)strtol(p, NULL, 10) - 1;       /* the file is 1-based */
            seen_port = 1;
            p = jskip(p);
        } else if (strcmp(key, "device") == 0) {
            p = jstring(p, value, sizeof value);
            if (!p)
                return NULL;
            set_device(&tmp, value);
        } else if (strcmp(key, "deadzone") == 0) {
            tmp.deadzone = (int)strtol(p, NULL, 10);
            p = jskip(p);
        } else if (strcmp(key, "bindings") == 0) {
            p = parse_bindings(p, &tmp, bad);
            if (!p)
                return NULL;
        } else {
            p = jskip(p);
            if (!p)
                return NULL;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (*p != '}')
        return NULL;
    if (port < 0 || port >= PORTS)
        return p + 1;                             /* a port nothing can read */
    /* "port" may come after "bindings", so the defaults this started from
     * could be the wrong port's. Only the pad index differs, and only when
     * the file did not say. */
    if (seen_port && tmp.device == DEV_XINPUT && tmp.pad == index && index != port)
        tmp.pad = port;
    tmp.has_key = 0;
    tmp.has_mouse = 0;
    for (i = 0; i < CONTROL_COUNT; i++) {
        int s;
        for (s = 0; s < tmp.bind[i].count; s++) {
            if (tmp.bind[i].src[s].kind == SRC_KEY)
                tmp.has_key = 1;
            if (tmp.bind[i].src[s].kind == SRC_MOUSE)
                tmp.has_mouse = 1;
        }
    }
    g_ctl[port] = tmp;
    return p + 1;
}

/* "true", "false", or a number: what a JSON switch may be written as. */
static int jbool(const char *p)
{
    if (strncmp(p, "true", 4) == 0 || strncmp(p, "on", 2) == 0)
        return 1;
    if (strncmp(p, "false", 5) == 0 || strncmp(p, "null", 4) == 0 ||
        strncmp(p, "off", 3) == 0)
        return 0;
    return strtol(p, NULL, 10) != 0;
}

/* "mouse": { "look": true, "sensitivity": 2.5, "invert_y": false }
 * Top level rather than per controller because there is one mouse, and its
 * look drives player 1. Sensitivity is in the usual PC-shooter unit:
 * 0.022 degrees per count at 1.0. */
static const char *parse_mouse(const char *p, RecompMouseConfig *m)
{
    char key[48];

    p = ws(p);
    if (*p != '{')
        return NULL;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p != ':')
            return NULL;
        p = ws(p + 1);
        if (strcmp(key, "look") == 0)
            m->look = jbool(p);
        else if (strcmp(key, "sensitivity") == 0)
            m->sensitivity = strtod(p, NULL);
        else if (strcmp(key, "invert_y") == 0)
            m->invert_y = jbool(p);
        p = jskip(p);
        if (!p)
            return NULL;
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    return *p == '}' ? p + 1 : NULL;
}

static int parse_config(const char *text)
{
    const char *p = ws(text);
    char key[48];
    int bad = 0, index = 0;

    if (*p != '{')
        return 0;
    p = ws(p + 1);
    while (*p && *p != '}') {
        p = jstring(p, key, sizeof key);
        if (!p)
            return 0;
        p = ws(p);
        if (*p != ':')
            return 0;
        p = ws(p + 1);
        if (strcmp(key, "controllers") == 0 && *p == '[') {
            p = ws(p + 1);
            while (*p && *p != ']') {
                p = parse_controller(p, index++, &bad);
                if (!p)
                    return 0;
                p = ws(p);
                if (*p == ',')
                    p = ws(p + 1);
            }
            if (*p != ']')
                return 0;
            p++;
        } else if (strcmp(key, "mouse") == 0 && *p == '{') {
            p = parse_mouse(p, &g_mouse);
            if (!p)
                return 0;
        } else {
            p = jskip(p);
            if (!p)
                return 0;
        }
        p = ws(p);
        if (*p == ',')
            p = ws(p + 1);
    }
    if (bad)
        fprintf(stderr, "[INPUT] %d binding(s) in the config file name a "
                        "control or a source this build does not know; "
                        "they are ignored\n", bad);
    return 1;
}

/* ---- where the config lives -------------------------------------------- */

static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* RECOMP_INPUT_CONFIG, then the per-user file the UI writes, then one beside
 * the executable (the working directory a title's run.bat starts in), so a
 * title can ship a config of its own. */
static int find_config(char *out, size_t n)
{
    const char *env = getenv("RECOMP_INPUT_CONFIG");
#if defined(_WIN32)
    const char *home = getenv("APPDATA");
    const char *tail = "\\xboxrecomp\\input_bindings.json";
#else
    const char *home = getenv("XDG_CONFIG_HOME");
    const char *tail = "/xboxrecomp/input_bindings.json";
    if (!home || !*home) {
        static char buf[400];
        const char *h = getenv("HOME");
        if (h) {
            snprintf(buf, sizeof buf, "%s/.config", h);
            home = buf;
        }
    }
#endif
    if (env && *env) {
        snprintf(out, n, "%s", env);
        if (readable(out))
            return 1;
        fprintf(stderr, "[INPUT] RECOMP_INPUT_CONFIG=%s cannot be read; "
                        "using the built-in bindings\n", env);
        return 0;
    }
    if (home && *home) {
        snprintf(out, n, "%s%s", home, tail);
        if (readable(out))
            return 1;
    }
    snprintf(out, n, "input_bindings.json");
    if (readable(out))
        return 1;
#if defined(_WIN32)
    /* Beside the executable itself, whatever the working directory: a
     * shortcut or a launcher may start it from somewhere else. */
    {
        char exe[MAX_PATH];
        DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = len && len < sizeof exe ? strrchr(exe, '\\') : NULL;
        if (slash) {
            *slash = '\0';
            snprintf(out, n, "%s\\input_bindings.json", exe);
            if (readable(out))
                return 1;
        }
    }
#endif
    return 0;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long size;
    char *buf;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20)) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void describe(Controller *c, char *out, size_t n)
{
    const char *mouse = c->has_mouse ? " + mouse" : "";

    if (c->device == DEV_KEYBOARD)
        snprintf(out, n, "keyboard%s", mouse);
    else if (c->device == DEV_XINPUT && c->has_key)
        snprintf(out, n, "XInput pad %d + keyboard%s", c->pad, mouse);
    else if (c->device == DEV_XINPUT)
        snprintf(out, n, "XInput pad %d%s", c->pad, mouse);
    else
        snprintf(out, n, "nothing");
}

static void mouse_defaults(RecompMouseConfig *m)
{
    memset(m, 0, sizeof *m);
    m->look = 1;            /* only matters to a title with a look hook */
    m->sensitivity = 2.5;
}

/* The environment over the file, as everywhere else in the runtime:
 * RECOMP_MOUSE=0 switches mouse look off, RECOMP_MOUSE_SENS and
 * RECOMP_MOUSE_INVERT_Y override the file's numbers. */
static void mouse_from_environment(RecompMouseConfig *m)
{
    const char *v;

    if ((v = getenv("RECOMP_MOUSE")) != NULL)
        m->look = !*v || jbool(v);
    if ((v = getenv("RECOMP_MOUSE_SENS")) != NULL && *v)
        m->sensitivity = strtod(v, NULL);
    if ((v = getenv("RECOMP_MOUSE_INVERT_Y")) != NULL)
        m->invert_y = !*v || jbool(v);
}

/* 0 not started, 1 loading, 2 loaded. The game window's thread asks for the
 * mouse settings while guest threads ask for the pads, so two threads can
 * arrive here together; the second waits for the first rather than reading
 * a half-parsed table. */
#if defined(_WIN32)
static volatile LONG g_init_state;
#else
static volatile long g_init_state;
#endif

static void load_bindings(void);

void recomp_bindings_init(void)
{
    if (g_init_state == 2)
        return;
#if defined(_WIN32)
    if (InterlockedCompareExchange(&g_init_state, 1, 0) != 0) {
        while (g_init_state != 2)
            Sleep(0);
        return;
    }
    load_bindings();
    InterlockedExchange(&g_init_state, 2);
#else
    if (!__sync_bool_compare_and_swap(&g_init_state, 0, 1)) {
        while (__atomic_load_n(&g_init_state, __ATOMIC_ACQUIRE) != 2)
            ;
        return;
    }
    load_bindings();
    __atomic_store_n(&g_init_state, 2, __ATOMIC_RELEASE);
#endif
}

static void load_bindings(void)
{
    char *text;
    int i;

    for (i = 0; i < PORTS; i++)
        defaults_for(&g_ctl[i], i);
    mouse_defaults(&g_mouse);
    if (find_config(g_path, sizeof g_path) && (text = read_file(g_path)) != NULL) {
        if (parse_config(text)) {
            g_have_path = 1;
        } else {
            fprintf(stderr, "[INPUT] %s is not a binding config this build "
                            "can read; using the built-in bindings\n", g_path);
            for (i = 0; i < PORTS; i++)
                defaults_for(&g_ctl[i], i);
            mouse_defaults(&g_mouse);
        }
        free(text);
    }
    mouse_from_environment(&g_mouse);
    for (i = 0; i < PORTS; i++)
        if (g_ctl[i].has_mouse)
            g_mouse.buttons_bound = 1;
    recomp_mouse_configure(&g_mouse);
    for (i = 0; i < PORTS; i++)
        describe(&g_ctl[i], g_ctl[i].label, sizeof g_ctl[i].label);
    fprintf(stderr, "[INPUT] bindings from %s\n",
            g_have_path ? g_path : "the built-in defaults (no config file)");
    for (i = 0; i < PORTS; i++)
        fprintf(stderr, "[INPUT]   controller %d: %s\n", i + 1, g_ctl[i].label);
    fflush(stderr);
}

const char *recomp_bindings_source_path(void)
{
    recomp_bindings_init();
    return g_have_path ? g_path : NULL;
}

const char *recomp_bindings_device_name(unsigned port)
{
    recomp_bindings_init();
    return port < PORTS ? g_ctl[port].label : "nothing";
}

/* ---- reading the host -------------------------------------------------- */

#if defined(_WIN32)

/* Whether this process owns the foreground window. GetAsyncKeyState and
 * XInputGetState are global: without this check the title reads every key
 * typed into another application, and a menu can act on them. Cached for a
 * few milliseconds because a title polls far more often than it draws.
 * RECOMP_INPUT_BACKGROUND=1 reads input regardless, for a title run from a
 * script or beside a debugger. */
static int window_has_focus(void)
{
    static int background = -1;
    static int focused = 1;
    static ULONGLONG checked_at;
    ULONGLONG now = GetTickCount64();

    if (background < 0) {
        const char *e = getenv("RECOMP_INPUT_BACKGROUND");
        background = e && *e && *e != '0';
    }
    if (background)
        return 1;
    if (now - checked_at >= 20u) {
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();

        if (fg)
            GetWindowThreadProcessId(fg, &pid);
        focused = pid == GetCurrentProcessId();
        checked_at = now;
    }
    return focused;
}

/* XInputGetState on an empty slot costs about a millisecond, and a title
 * polls its pads several times a frame: an unplugged port would cost more
 * than the game. An empty slot is re-checked once a second, a full one every
 * poll. */
static int pad_state(int index, XINPUT_STATE *out)
{
    static XINPUT_STATE cached[PORTS];
    static int connected[PORTS];
    static ULONGLONG retry_at[PORTS];
    ULONGLONG now = GetTickCount64();

    if (index < 0 || index >= PORTS || !window_has_focus())
        return 0;
    if (!connected[index] && now < retry_at[index])
        return 0;
    memset(&cached[index], 0, sizeof cached[index]);
    if (XInputGetState((DWORD)index, &cached[index]) == ERROR_SUCCESS) {
        connected[index] = 1;
    } else {
        connected[index] = 0;
        retry_at[index] = now + 1000;
    }
    *out = cached[index];
    return connected[index];
}

static const WORD PAD_BITS[PB_COUNT] = {
    XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
    XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT
};

/* One answer per key per sample: 24 controls can name the same key twice and
 * GetAsyncKeyState is a call into the window manager.
 *
 * Per sample is not enough on its own. A title is free to read the pad far
 * more often than it draws, and each read was costing a window-manager
 * transition per distinct key: Tony Hawk's Pro Skater 2X polls about 372,000
 * times a second and spent 47% of its main thread inside
 * NtUserGetAsyncKeyState -- four times what it spent in its own lifted code.
 *
 * So the answers also persist between samples for a short while. A keyboard
 * cannot change faster than a person can move, the console's own pad read is
 * a USB transfer that returns state captured up to a frame earlier, and one
 * millisecond is below anything a player can perceive. It is deliberately
 * not tied to the frame: a title that polls without drawing still has to get
 * fresh input eventually, or a menu that spins waiting for a key would never
 * see one.
 */
#define KEY_CACHE_TTL_MS 1u

typedef struct { unsigned char state[256]; } KeyCache;

static unsigned char g_key_state[256];
static DWORD g_key_state_at;

static int key_down(KeyCache *kc, int vk)
{
    (void)kc;
    if (vk <= 0 || vk > 255 || !window_has_focus())
        return 0;
    if (!g_key_state[vk])
        g_key_state[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 2 : 1;
    return g_key_state[vk] == 2;
}

/* Start of a sample: drop answers older than the window above. */
static void key_cache_tick(void)
{
    DWORD now = GetTickCount();
    if (now - g_key_state_at >= KEY_CACHE_TTL_MS) {
        memset(g_key_state, 0, sizeof g_key_state);
        g_key_state_at = now;
    }
}

static int axis_value(const XINPUT_GAMEPAD *g, int axis)
{
    switch (axis) {
    case AXIS_LX: return g->sThumbLX;
    case AXIS_LY: return g->sThumbLY;
    case AXIS_RX: return g->sThumbRX;
    default:      return g->sThumbRY;
    }
}

/* Every source reads as a 0..255 magnitude, whatever it is: that is what
 * makes a trigger bindable to a key and a button bindable to a stick. */
static int source_magnitude(const Source *s, KeyCache *kc,
                            const XINPUT_STATE *pad, int have_pad, int deadzone)
{
    switch (s->kind) {
    case SRC_KEY:
        return key_down(kc, s->arg) ? 255 : 0;
    case SRC_PAD_BUTTON:
        if (!have_pad)
            return 0;
        return (pad->Gamepad.wButtons & PAD_BITS[s->arg]) ? 255 : 0;
    case SRC_PAD_TRIGGER: {
        /* A resting XInput trigger can read a few units above zero, and a
         * title that treats any non-zero analog button as pressed would
         * fire forever. Below XInput's own recommended threshold is "not
         * pressed"; above it the value passes through untouched. */
        int t;
        if (!have_pad)
            return 0;
        t = s->arg ? pad->Gamepad.bRightTrigger : pad->Gamepad.bLeftTrigger;
        return t < TRIGGER_THRESHOLD ? 0 : t;
    }
    case SRC_MOUSE:
        return recomp_mouse_source(s->arg);
    case SRC_PAD_AXIS: {
        int v, span;
        if (!have_pad)
            return 0;
        v = axis_value(&pad->Gamepad, s->arg) * s->sign;
        if (v <= deadzone)
            return 0;
        if (v > 32767)
            v = 32767;
        span = 32767 - deadzone;
        if (span <= 0)
            return 255;
        return (v - deadzone) * 255 / span;
    }
    default:
        return 0;
    }
}

/* The first thing pressed on a port, named once. A binding that is wrong is
 * otherwise invisible: the title just never responds, and there is no way to
 * tell a mis-bound key from a title that is not reading the pad at all. */
static void say_first_press(unsigned port, const char *control)
{
    static int said[PORTS];

    if (port < PORTS && !said[port]) {
        said[port] = 1;
        fprintf(stderr, "[INPUT] port %u first press: %s (%s)\n", port + 1,
                control, g_ctl[port].label);
        fflush(stderr);
    }
}

int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out)
{
    Controller *c;
    KeyCache kc;
    XINPUT_STATE pad;
    int have_pad = 0, i;
    int axis[4] = { 0, 0, 0, 0 };

    if (!out)
        return 0;
    recomp_bindings_init();
    memset(out, 0, sizeof *out);
    if (port >= PORTS)
        return 0;
    c = &g_ctl[port];
    if (c->device == DEV_NONE)
        return 0;
    memset(&kc, 0, sizeof kc);
    key_cache_tick();
    memset(&pad, 0, sizeof pad);
    if (c->device == DEV_XINPUT)
        have_pad = pad_state(c->pad, &pad);

    for (i = 0; i < CONTROL_COUNT; i++) {
        int mag = 0, s;

        for (s = 0; s < c->bind[i].count; s++) {
            int m = source_magnitude(&c->bind[i].src[s], &kc, &pad, have_pad,
                                     c->deadzone);
            if (m > mag)
                mag = m;
        }
        if (!mag)
            continue;
        say_first_press(port, CONTROLS[i].name);
        switch (CONTROLS[i].kind) {
        case K_DIGITAL:
            if (mag >= 64)
                out->wButtons |= CONTROLS[i].arg;
            break;
        case K_ANALOG:
            if (mag > out->bAnalogButtons[CONTROLS[i].arg])
                out->bAnalogButtons[CONTROLS[i].arg] = (BYTE)mag;
            break;
        default:
            axis[CONTROLS[i].arg] += CONTROLS[i].sign * mag;
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        long v;
        if (axis[i] > 255)  axis[i] = 255;
        if (axis[i] < -255) axis[i] = -255;
        v = (long)axis[i] * 32767 / 255;
        switch (i) {
        case AXIS_LX: out->sThumbLX = (SHORT)v; break;
        case AXIS_LY: out->sThumbLY = (SHORT)v; break;
        case AXIS_RX: out->sThumbRX = (SHORT)v; break;
        default:      out->sThumbRY = (SHORT)v; break;
        }
    }
    return 1;
}

unsigned recomp_bindings_present_mask(void)
{
    static ULONGLONG next;
    static unsigned mask;
    ULONGLONG now;
    int i;

    recomp_bindings_init();
    now = GetTickCount64();
    if (mask && now < next)
        return mask;
    next = now + 250;
    mask = 0;
    for (i = 0; i < PORTS; i++) {
        Controller *c = &g_ctl[i];
        if (c->device == DEV_KEYBOARD ||
            (c->device == DEV_XINPUT && (c->has_key || c->has_mouse))) {
            mask |= 1u << i;
        } else if (c->device == DEV_XINPUT) {
            XINPUT_STATE st;
            if (pad_state(c->pad, &st))
                mask |= 1u << i;
        }
    }
    return mask;
}

#else /* not Windows: nothing to read from yet, but the config still parses */

int recomp_bindings_sample(unsigned port, XBOX_GAMEPAD *out)
{
    recomp_bindings_init();
    if (!out)
        return 0;
    memset(out, 0, sizeof *out);
    (void)port;
    return 0;
}

unsigned recomp_bindings_present_mask(void)
{
    recomp_bindings_init();
    return g_ctl[0].device != DEV_NONE ? 1u : 0u;
}

#endif
