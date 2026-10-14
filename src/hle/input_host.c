/*
 * input_host.c -- the host controller behind the replaced XAPI input.
 *
 * Adapted from doaxbv-re (https://github.com/NoRain211/doaxbv-re,
 * recomp-runtime/input_host_win32.c), GPL-3.0: XInput pad 0 and the keyboard,
 * mapped to the Xbox gamepad. Added here: RECOMP_FAKE_INPUT, the scripted
 * presses src/usb/usb_gamepad.c already understood, so an unattended run can
 * still get past "Press START"; and RECOMP_INPUT_SEQ, a one-shot timed
 * sequence that walks a menu path the same way every run.
 *
 * Which host device drives which of the four ports, and which key or pad
 * control drives each Xbox input, is src/input/input_bindings.c reading the
 * config file the input UI writes (`py -3 -m tools.input_ui`). With no config
 * file the defaults are what this file used to hard-code: XInput pad 0 plus
 * the keyboard on port 1 -- arrows = D-pad, Enter = START, Backspace = BACK,
 * Z = A, X = B, A = X, S = Y, Q = White, W = Black, E/R = triggers -- and
 * XInput pad N on port N.
 *
 * RECOMP_FAKE_INPUT and RECOMP_INPUT_SEQ still press buttons directly, on
 * port 1 only: they are a substitute for a person at the keyboard, so they
 * are deliberately unaffected by what that person bound.
 */
#include "input_host.h"
#include "input_bindings.h"
#include "recomp_mouse.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    XBOX_DPAD_UP = 0x0001u,
    XBOX_DPAD_DOWN = 0x0002u,
    XBOX_DPAD_LEFT = 0x0004u,
    XBOX_DPAD_RIGHT = 0x0008u,
    XBOX_START = 0x0010u,
    XBOX_BACK = 0x0020u,
};

/* RECOMP_FAKE_INPUT=start,a  presses each listed button in turn, one per
 * cycle; RECOMP_FAKE_INPUT_MS (default 500) is how long each press and each
 * gap lasts. Pressed and released rather than held: menus act on the edge. */
/* kind: 0 a button, 1 an analog button, 2 a stick pushed fully one way.
 * The sticks are here because a script that can only press buttons can reach
 * a menu but not a place in a level, and "walk to where it looks wrong" is
 * how a rendering bug gets reproduced without a person at the pad. The names
 * match the controls in the binding config (src/input/input_bindings.c). */
enum { FAKE_BUTTON, FAKE_ANALOG, FAKE_STICK, FAKE_MOUSE, FAKE_CLICK };
enum { STICK_LX, STICK_LY, STICK_RX, STICK_RY };
/* FAKE_MOUSE: mouse movement while held, at SEQ_MOUSE_RATE counts a second
 * (RECOMP_INPUT_SEQ_MOUSE_RATE overrides), for a title with a mouse-look hook. */
enum { MOUSE_DIR_X, MOUSE_DIR_Y };
#define SEQ_MOUSE_RATE 1000.0

static const struct { const char *name; int kind; unsigned value; int sign; } FAKE[] = {
    { "start", FAKE_BUTTON, XBOX_START, 0 },      { "back",  FAKE_BUTTON, XBOX_BACK, 0 },
    { "up",    FAKE_BUTTON, XBOX_DPAD_UP, 0 },    { "down",  FAKE_BUTTON, XBOX_DPAD_DOWN, 0 },
    { "left",  FAKE_BUTTON, XBOX_DPAD_LEFT, 0 },  { "right", FAKE_BUTTON, XBOX_DPAD_RIGHT, 0 },
    { "a",     FAKE_ANALOG, HOST_ANALOG_A, 0 },   { "b",     FAKE_ANALOG, HOST_ANALOG_B, 0 },
    { "x",     FAKE_ANALOG, HOST_ANALOG_X, 0 },   { "y",     FAKE_ANALOG, HOST_ANALOG_Y, 0 },
    { "white", FAKE_ANALOG, HOST_ANALOG_WHITE, 0 }, { "black", FAKE_ANALOG, HOST_ANALOG_BLACK, 0 },
    { "lt",    FAKE_ANALOG, HOST_ANALOG_LTRIG, 0 }, { "rt",    FAKE_ANALOG, HOST_ANALOG_RTRIG, 0 },
    { "lstick_up",    FAKE_STICK, STICK_LY,  1 }, { "lstick_down",  FAKE_STICK, STICK_LY, -1 },
    { "lstick_left",  FAKE_STICK, STICK_LX, -1 }, { "lstick_right", FAKE_STICK, STICK_LX,  1 },
    { "rstick_up",    FAKE_STICK, STICK_RY,  1 }, { "rstick_down",  FAKE_STICK, STICK_RY, -1 },
    { "rstick_left",  FAKE_STICK, STICK_RX, -1 }, { "rstick_right", FAKE_STICK, STICK_RX,  1 },
    { "mouse_up",     FAKE_MOUSE, MOUSE_DIR_Y, -1 }, { "mouse_down",  FAKE_MOUSE, MOUSE_DIR_Y,  1 },
    { "mouse_left",   FAKE_MOUSE, MOUSE_DIR_X, -1 }, { "mouse_right", FAKE_MOUSE, MOUSE_DIR_X,  1 },
    { "click",        FAKE_CLICK, RECOMP_MOUSE_LEFT, 0 }, { "rclick",    FAKE_CLICK, RECOMP_MOUSE_RIGHT, 0 },
    { "wheel_up",     FAKE_CLICK, RECOMP_MOUSE_WHEEL_UP, 0 }, { "wheel_down", FAKE_CLICK, RECOMP_MOUSE_WHEEL_DOWN, 0 },
};

/* Name -> FAKE index, or -1. `n` is the name's length inside a longer spec. */
static int fake_lookup(const char *p, size_t n)
{
    int i;
    for (i = 0; i < (int)(sizeof FAKE / sizeof FAKE[0]); i++)
        if (strlen(FAKE[i].name) == n && strncmp(p, FAKE[i].name, n) == 0)
            return i;
    return -1;
}

static void fake_apply(RecompInputGamepad *g, int i)
{
    int16_t push = (int16_t)(FAKE[i].sign > 0 ? 32767 : -32767);

    switch (FAKE[i].kind) {
    case FAKE_ANALOG:
        g->analog_buttons[FAKE[i].value] = 0xFFu;
        break;
    case FAKE_STICK:
        switch (FAKE[i].value) {
        case STICK_LX: g->thumb_lx = push; break;
        case STICK_LY: g->thumb_ly = push; break;
        case STICK_RX: g->thumb_rx = push; break;
        default:       g->thumb_ry = push; break;
        }
        break;
    case FAKE_MOUSE:
    case FAKE_CLICK:
        break;                  /* fed by time in seq_input, not per sample */
    default:
        g->buttons |= (uint16_t)FAKE[i].value;
        break;
    }
}

/* RECOMP_INPUT_SEQ=1500:start,4000:a,6000:down+a:400 -- a one-shot script.
 *
 * Each step is <ms>:<control>[+<control>...][:<hold ms>]. A control is a
 * button, an analog button, or a stick direction held all the way over
 * (lstick_up, rstick_left, ...), so a script can walk and turn, not only
 * press. The clock starts the
 * first time the title reads the pad, so a slow boot does not eat the script;
 * hold defaults to 200 ms, long enough for a title polling at 60 Hz to see the
 * press and the release. Steps may overlap, which is how a chord is held.
 *
 * This exists because RECOMP_FAKE_INPUT cycles forever: it is right for
 * "get past Press START" and wrong for a menu, where it keeps pressing things
 * at whatever screen it reached. A menu path is a fixed sequence, and there is
 * no save state to restore a native process to -- lifted code is mid-flight
 * on real threads with host GPU objects behind it -- so replaying the presses
 * is how a run gets back to the same screen. The sequence is the save state.
 * Nothing is pressed after the last step ends. */
#define SEQ_MAX 128
/* ptr_<x>_<y> (a step of its own or with others): the menu pointer to x%, y%
 * of the picture, for a title whose menus take the mouse as a pointer; click
 * and rclick press the left and right buttons there, wheel_up and wheel_down
 * turn the wheel a notch (recomp_mouse.h). */
static struct seq_step {
    unsigned long at, hold; int keys[4]; int nkeys;
    int has_ptr; float ptr_x, ptr_y;
} s_seq[SEQ_MAX];
static int s_seq_count = -1;
static unsigned long long s_seq_t0;

static void seq_parse(const char *env)
{
    const char *p = env;

    while (*p && s_seq_count < SEQ_MAX) {
        struct seq_step *s = &s_seq[s_seq_count];
        char *end;
        const char *step_end = strchr(p, ',');
        size_t len = step_end ? (size_t)(step_end - p) : strlen(p);
        const char *q;

        memset(s, 0, sizeof *s);
        s->hold = 200;
        s->at = strtoul(p, &end, 10);
        if (end == p || *end != ':') {
            fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: bad step at '%.*s' (want <ms>:<button>)\n",
                    (int)len, p);
            return;
        }
        q = end + 1;
        while (q < p + len) {
            size_t n = 0;
            int k;
            while (q + n < p + len && q[n] != '+' && q[n] != ':')
                n++;
            if (n > 4 && strncmp(q, "ptr_", 4) == 0) {
                float x, y;
                if (sscanf(q + 4, "%f_%f", &x, &y) != 2) {
                    fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: bad pointer '%.*s' (want ptr_<x%%>_<y%%>)\n",
                            (int)n, q);
                    return;
                }
                s->has_ptr = 1;
                s->ptr_x = x / 100.0f;
                s->ptr_y = y / 100.0f;
                q += n;
                if (q < p + len && *q == '+')
                    q++;
                continue;
            }
            k = fake_lookup(q, n);
            if (k < 0) {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: unknown button '%.*s'\n", (int)n, q);
                return;
            }
            if (s->nkeys < 4)
                s->keys[s->nkeys++] = k;
            q += n;
            if (q < p + len && *q == ':') {
                s->hold = strtoul(q + 1, NULL, 10);
                if (!s->hold)
                    s->hold = 200;
                break;
            }
            if (q < p + len && *q == '+')
                q++;
        }
        s_seq_count++;
        if (!step_end)
            break;
        p = step_end + 1;
    }
}

/* Milliseconds since the title first read a pad: the clock RECOMP_INPUT_SEQ,
 * RECOMP_INPUT_RECORD and RECOMP_INPUT_REPLAY all share, so a recording and
 * a script line up with each other however long the boot took. */
static unsigned long long input_clock_ms(void)
{
    if (!s_seq_t0)
        s_seq_t0 = GetTickCount64();
    return GetTickCount64() - s_seq_t0;
}

/* "@path": the script is in a file, one or more steps a line, '#' starting a
 * comment. Whitespace is dropped, so the file can be laid out by hand. The
 * result is the same comma-separated text the variable takes. */
static char *seq_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *out, line[1024];
    size_t cap = 4096, len = 0;

    if (!f) {
        fprintf(stderr, "  [PAD] RECOMP_INPUT_SEQ: cannot open %s\n", path);
        return NULL;
    }
    out = (char *)malloc(cap);
    if (!out) {
        fclose(f);
        return NULL;
    }
    out[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *c;
        for (c = line; *c && *c != '#'; c++) {
            if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n')
                continue;
            if (len + 2 >= cap) {
                char *grown = (char *)realloc(out, cap * 2);
                if (!grown)
                    break;
                out = grown;
                cap *= 2;
            }
            out[len++] = *c;
        }
        /* A line ends a step even without its comma. */
        if (len && out[len - 1] != ',' && len + 2 < cap)
            out[len++] = ',';
        out[len] = 0;
    }
    fclose(f);
    while (len && out[len - 1] == ',')
        out[--len] = 0;
    return out;
}

static void seq_input(RecompInputGamepad *g)
{
    static unsigned long long prev_t;
    static double mouse_rate, carry[2];
    unsigned long long t;
    int i, k;

    if (s_seq_count < 0) {
        const char *env = getenv("RECOMP_INPUT_SEQ");
        const char *rate = getenv("RECOMP_INPUT_SEQ_MOUSE_RATE");
        s_seq_count = 0;
        mouse_rate = rate && *rate ? strtod(rate, NULL) : SEQ_MOUSE_RATE;
        if (env && *env == '@') {
            char *text = seq_from_file(env + 1);
            if (text) {
                seq_parse(text);
                free(text);
            }
            fprintf(stderr, "  [PAD] input script: %d steps from %s\n", s_seq_count, env + 1);
            fflush(stderr);
        } else if (env && *env) {
            seq_parse(env);
            fprintf(stderr, "  [PAD] input script: %d steps from %s\n", s_seq_count, env);
            fflush(stderr);
        }
    }
    if (!s_seq_count)
        return;
    t = input_clock_ms();
    for (i = 0; i < s_seq_count; i++) {
        unsigned long long end = (unsigned long long)s_seq[i].at + s_seq[i].hold;
        if (t < s_seq[i].at || prev_t >= end)
            continue;
        /* The pointer is placed, and a click pressed, once at the start of
         * the step; the click is let go at its end. */
        if (s_seq[i].has_ptr && (prev_t < s_seq[i].at || !prev_t))
            recomp_mouse_script_pointer(s_seq[i].ptr_x, s_seq[i].ptr_y);
        for (k = 0; k < s_seq[i].nkeys; k++) {
            int f = s_seq[i].keys[k];
            if (FAKE[f].kind == FAKE_CLICK) {
                if (prev_t < s_seq[i].at || !prev_t)
                    recomp_mouse_script_button((int)FAKE[f].value, 1);
                if (t >= end)
                    recomp_mouse_script_button((int)FAKE[f].value, 0);
            } else if (FAKE[f].kind == FAKE_MOUSE) {
                /* The part of this step's hold that fell since the last
                 * sample, as counts: the same total however often the
                 * title polls. */
                unsigned long long from = prev_t > s_seq[i].at ? prev_t : s_seq[i].at;
                unsigned long long to = t < end ? t : end;
                double counts;
                long whole;
                if (to <= from)
                    continue;
                counts = (double)(to - from) * mouse_rate / 1000.0 * FAKE[f].sign;
                carry[FAKE[f].value] += counts;
                whole = (long)carry[FAKE[f].value];
                carry[FAKE[f].value] -= (double)whole;
                if (FAKE[f].value == MOUSE_DIR_X)
                    recomp_mouse_feed_motion(whole, 0);
                else
                    recomp_mouse_feed_motion(0, whole);
            } else if (t < end) {
                fake_apply(g, f);
            }
        }
    }
    prev_t = t;
}

/* RECOMP_INPUT_RECORD=<file>: what player 1 does, written as it happens, for
 * RECOMP_INPUT_REPLAY to play back. The way to get back to a place in the
 * game without walking there by hand every run: record the menus once (or
 * the whole route), and replay it.
 *
 * One line per change of the pad, "<ms> <buttons> <8 analog bytes> <lx> <ly>
 * <rx> <ry>", and "<ms> m <dx> <dy>" for mouse movement, on the clock above.
 * Only port 1, as for the scripts. What is recorded is what the title read,
 * scripted presses included, so a recording made while a RECOMP_INPUT_SEQ
 * runs holds both. */
static FILE *s_rec;

static void record_input(const RecompInputGamepad *g)
{
    static int init;
    static RecompInputGamepad last;
    static int have_last;
    unsigned long long t;
    long dx, dy;

    if (!init) {
        const char *path = getenv("RECOMP_INPUT_RECORD");
        init = 1;
        if (path && *path) {
            s_rec = fopen(path, "w");
            if (s_rec) {
                fprintf(s_rec, "# xboxrecomp input recording v1: <ms> <buttons> "
                               "<a b x y black white lt rt> <lx ly rx ry> | <ms> m <dx> <dy>\n");
                recomp_mouse_record_motion(1);
                fprintf(stderr, "  [PAD] recording player 1's input to %s\n", path);
            } else {
                fprintf(stderr, "  [PAD] RECOMP_INPUT_RECORD: cannot write %s\n", path);
            }
            fflush(stderr);
        }
    }
    if (!s_rec)
        return;
    t = input_clock_ms();
    if (recomp_mouse_take_recorded(&dx, &dy))
        fprintf(s_rec, "%llu m %ld %ld\n", t, dx, dy);
    if (have_last && memcmp(&last, g, sizeof last) == 0)
        return;
    last = *g;
    have_last = 1;
    fprintf(s_rec, "%llu %04X %u %u %u %u %u %u %u %u %d %d %d %d\n", t, g->buttons,
            g->analog_buttons[0], g->analog_buttons[1], g->analog_buttons[2],
            g->analog_buttons[3], g->analog_buttons[4], g->analog_buttons[5],
            g->analog_buttons[6], g->analog_buttons[7],
            g->thumb_lx, g->thumb_ly, g->thumb_rx, g->thumb_ry);
    fflush(s_rec);
}

/* RECOMP_INPUT_REPLAY=<file>: a RECOMP_INPUT_RECORD file played back on
 * port 1. While it lasts it replaces the live pad (a replay that a stray key
 * can push off course is no use); after its last line the live pad is back,
 * so a replay that ends in a level hands the game over to the player. The
 * title is not deterministic -- loading times, threads -- so menus replay
 * reliably and long stretches of play drift; record up to where control is
 * needed, not beyond. */
typedef struct { unsigned long long at; int mouse; long dx, dy; RecompInputGamepad g; } ReplayLine;
static ReplayLine *s_rep;
static size_t s_rep_count, s_rep_next;
static RecompInputGamepad s_rep_pad;

static void replay_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    size_t cap = 0;

    if (!f) {
        fprintf(stderr, "  [PAD] RECOMP_INPUT_REPLAY: cannot open %s\n", path);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        ReplayLine r;
        unsigned b, a[8];
        int lx, ly, rx, ry, i;

        memset(&r, 0, sizeof r);
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%llu m %ld %ld", &r.at, &r.dx, &r.dy) == 3) {
            r.mouse = 1;
        } else if (sscanf(line, "%llu %x %u %u %u %u %u %u %u %u %d %d %d %d", &r.at, &b,
                          &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7],
                          &lx, &ly, &rx, &ry) == 14) {
            r.g.buttons = (uint16_t)b;
            for (i = 0; i < 8; i++)
                r.g.analog_buttons[i] = (uint8_t)a[i];
            r.g.thumb_lx = (int16_t)lx;
            r.g.thumb_ly = (int16_t)ly;
            r.g.thumb_rx = (int16_t)rx;
            r.g.thumb_ry = (int16_t)ry;
        } else {
            continue;
        }
        if (s_rep_count == cap) {
            ReplayLine *grown;
            cap = cap ? cap * 2 : 1024;
            grown = (ReplayLine *)realloc(s_rep, cap * sizeof *s_rep);
            if (!grown)
                break;
            s_rep = grown;
        }
        s_rep[s_rep_count++] = r;
    }
    fclose(f);
    fprintf(stderr, "  [PAD] replaying %u input changes from %s (%.1f s)\n",
            (unsigned)s_rep_count, path,
            s_rep_count ? (double)s_rep[s_rep_count - 1].at / 1000.0 : 0.0);
    fflush(stderr);
}

/* Returns 1 while the replay is running (and *g is its pad). */
static int replay_input(RecompInputGamepad *g)
{
    static int init;
    unsigned long long t;

    if (!init) {
        const char *path = getenv("RECOMP_INPUT_REPLAY");
        init = 1;
        if (path && *path)
            replay_load(path);
    }
    if (!s_rep_count)
        return 0;
    t = input_clock_ms();
    while (s_rep_next < s_rep_count && s_rep[s_rep_next].at <= t) {
        const ReplayLine *r = &s_rep[s_rep_next++];
        if (r->mouse)
            recomp_mouse_feed_motion(r->dx, r->dy);
        else
            s_rep_pad = r->g;
    }
    if (s_rep_next >= s_rep_count) {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "  [PAD] replay finished at %.1f s: the pad is live\n",
                    (double)t / 1000.0);
            fflush(stderr);
        }
        return 0;
    }
    *g = s_rep_pad;
    return 1;
}

static void fake_input(RecompInputGamepad *g)
{
    static int checked;
    static char spec[64];
    static unsigned long period;
    const char *p;
    unsigned long long now;
    int count = 1, want, index = 0, i;

    if (!checked) {
        const char *env = getenv("RECOMP_FAKE_INPUT");
        const char *ms = getenv("RECOMP_FAKE_INPUT_MS");
        checked = 1;
        if (env) {
            strncpy(spec, env, sizeof spec - 1);
            fprintf(stderr, "  [PAD] synthetic input: %s\n", spec);
            fflush(stderr);
        }
        period = ms ? strtoul(ms, NULL, 0) : 0;
        if (!period)
            period = 500;
    }
    if (!spec[0])
        return;
    now = GetTickCount64();
    if ((now / period) % 2 == 0)
        return;                                 /* the gap half of the cycle */
    for (p = spec; *p; p++)
        if (*p == ',')
            count++;
    want = (int)((now / (period * 2)) % (unsigned long long)count);
    p = spec;
    while (index < want && (p = strchr(p, ',')) != NULL) {
        p++;
        index++;
    }
    if (!p)
        return;
    {
        const char *e = strchr(p, ',');
        i = fake_lookup(p, e ? (size_t)(e - p) : strlen(p));
        if (i >= 0)
            fake_apply(g, i);
    }
}

/* One port, through its bindings. The bound sample arrives as the layout
 * src/input already speaks (XBOX_GAMEPAD); the two structures are the same
 * eight analog bytes and four axes in the same order, so this is a copy. */
bool recomp_input_host_sample_port(unsigned port, RecompInputGamepad *gamepad)
{
    XBOX_GAMEPAD pad;
    int i;

    if (gamepad == NULL)
        return false;
    memset(gamepad, 0, sizeof *gamepad);
    if (recomp_bindings_sample(port, &pad)) {
        gamepad->buttons = (uint16_t)pad.wButtons;
        for (i = 0; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; i++)
            gamepad->analog_buttons[i] = pad.bAnalogButtons[i];
        gamepad->thumb_lx = (int16_t)pad.sThumbLX;
        gamepad->thumb_ly = (int16_t)pad.sThumbLY;
        gamepad->thumb_rx = (int16_t)pad.sThumbRX;
        gamepad->thumb_ry = (int16_t)pad.sThumbRY;
    }
    if (port == 0) {
        replay_input(gamepad);
        fake_input(gamepad);
        seq_input(gamepad);
        record_input(gamepad);
    }
    return true;
}

bool recomp_input_host_sample(RecompInputGamepad *gamepad)
{
    return recomp_input_host_sample_port(0u, gamepad);
}
