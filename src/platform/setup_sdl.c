/*
 * setup_sdl.c -- the first-run window (recomp_setup.h) on SDL2, with text
 * drawn through FreeType in the desktop's sans-serif font (fontconfig).
 *
 * Kept deliberately plain: a heading, a paragraph, at most one text field,
 * a progress bar and a row of buttons. Mouse and keyboard both work: Enter
 * is the highlighted button, Escape quits.
 *
 * The file dialog is the desktop's (kdialog on KDE, else zenity, run on a
 * thread so the window keeps drawing); without either the player types,
 * pastes or drops the path.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "recomp_setup.h"

#include <SDL.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define WIN_W 780
#define WIN_H 460
#define PAD   32

/* Colours: a dark window like the game's own menus. */
typedef struct { Uint8 r, g, b; } Rgb;
static const Rgb C_BG      = { 0x16, 0x18, 0x1d };
static const Rgb C_PANEL   = { 0x22, 0x25, 0x2c };
static const Rgb C_TEXT    = { 0xe8, 0xea, 0xee };
static const Rgb C_DIM     = { 0x8d, 0x93, 0x9e };
static const Rgb C_ACCENT  = { 0xe0, 0x8a, 0x1e };
static const Rgb C_ERROR   = { 0xff, 0x6b, 0x5e };
static const Rgb C_FIELD   = { 0x0f, 0x11, 0x14 };
static const Rgb C_BUTTON  = { 0x33, 0x37, 0x40 };
static const Rgb C_HOVER   = { 0x44, 0x49, 0x55 };

enum { F_BODY, F_BOLD, F_HEAD, F_SMALL, F_COUNT };
static const int k_font_px[F_COUNT] = { 16, 16, 24, 13 };

typedef struct Glyph {
    unsigned cp;
    int font;
    SDL_Texture *tex;
    int w, h, left, top, advance;
    struct Glyph *next;
} Glyph;

static struct {
    int open;
    SDL_Window *win;
    SDL_Renderer *ren;
    float scale;                        /* drawable pixels per window unit */
    FT_Library ft;
    FT_Face face[F_COUNT];
    Glyph *glyphs[256];
    int ascent[F_COUNT], line_h[F_COUNT];
    char title[128];
} S;

/* ------------------------------------------------------------------ fonts */

static int font_file(const char *pattern, char *out, size_t n)
{
    FcPattern *pat, *match;
    FcResult res;
    FcChar8 *file = NULL;
    out[0] = '\0';
    if (!FcInit() || !(pat = FcNameParse((const FcChar8 *)pattern)))
        return 0;
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    match = FcFontMatch(NULL, pat, &res);
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
        snprintf(out, n, "%s", (const char *)file);
    if (match)
        FcPatternDestroy(match);
    FcPatternDestroy(pat);
    return out[0] != '\0';
}

static int load_fonts(void)
{
    char regular[512], bold[512];
    int i;
    if (FT_Init_FreeType(&S.ft) != 0)
        return 0;
    if (!font_file("sans-serif", regular, sizeof regular))
        return 0;
    if (!font_file("sans-serif:weight=bold", bold, sizeof bold))
        snprintf(bold, sizeof bold, "%s", regular);
    for (i = 0; i < F_COUNT; i++) {
        const char *file = (i == F_BOLD || i == F_HEAD) ? bold : regular;
        if (FT_New_Face(S.ft, file, 0, &S.face[i]) != 0)
            return 0;
        FT_Set_Pixel_Sizes(S.face[i], 0, (FT_UInt)lroundf(k_font_px[i] * S.scale));
        S.ascent[i] = (int)(S.face[i]->size->metrics.ascender >> 6);
        S.line_h[i] = (int)(S.face[i]->size->metrics.height >> 6);
    }
    return 1;
}

static Glyph *glyph(int font, unsigned cp)
{
    unsigned slot = (cp * 31u + (unsigned)font) & 255u;
    Glyph *g;
    FT_GlyphSlot gs;
    for (g = S.glyphs[slot]; g; g = g->next)
        if (g->cp == cp && g->font == font)
            return g;
    if (FT_Load_Char(S.face[font], cp, FT_LOAD_RENDER) != 0 &&
        FT_Load_Char(S.face[font], '?', FT_LOAD_RENDER) != 0)
        return NULL;
    gs = S.face[font]->glyph;
    g = (Glyph *)calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->cp = cp;
    g->font = font;
    g->w = (int)gs->bitmap.width;
    g->h = (int)gs->bitmap.rows;
    g->left = gs->bitmap_left;
    g->top = gs->bitmap_top;
    g->advance = (int)(gs->advance.x >> 6);
    if (g->w > 0 && g->h > 0) {
        Uint32 *px = (Uint32 *)malloc((size_t)g->w * g->h * 4);
        int x, y;
        if (px) {
            for (y = 0; y < g->h; y++)
                for (x = 0; x < g->w; x++) {
                    Uint8 a = gs->bitmap.buffer[y * gs->bitmap.pitch + x];
                    px[y * g->w + x] = ((Uint32)a << 24) | 0x00FFFFFFu;
                }
            g->tex = SDL_CreateTexture(S.ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, g->w, g->h);
            if (g->tex) {
                SDL_UpdateTexture(g->tex, NULL, px, g->w * 4);
                SDL_SetTextureBlendMode(g->tex, SDL_BLENDMODE_BLEND);
            }
            free(px);
        }
    }
    g->next = S.glyphs[slot];
    S.glyphs[slot] = g;
    return g;
}

/* One code point from UTF-8, advancing *s. Bad bytes read as '?'. */
static unsigned utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp;
    int n, i;
    if (p[0] < 0x80) { *s += 1; return p[0]; }
    if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 1; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 2; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 3; }
    else { *s += 1; return '?'; }
    for (i = 1; i <= n; i++) {
        if ((p[i] & 0xC0) != 0x80) { *s += 1; return '?'; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *s += n + 1;
    return cp;
}

/* Width in drawable pixels of n bytes of s. */
static int text_width(int font, const char *s, size_t n)
{
    const char *end = s + n;
    int w = 0;
    while (s < end && *s) {
        Glyph *g = glyph(font, utf8_next(&s));
        if (g)
            w += g->advance;
    }
    return w;
}

/* Draw n bytes of s with the top of the line at (x, y), window units. */
static int draw_text_n(int font, int x, int y, Rgb c, const char *s, size_t n)
{
    const char *end = s + n;
    int px = (int)lroundf(x * S.scale), py = (int)lroundf(y * S.scale) + S.ascent[font];
    while (s < end && *s) {
        Glyph *g = glyph(font, utf8_next(&s));
        if (!g)
            continue;
        if (g->tex) {
            SDL_Rect r = { px + g->left, py - g->top, g->w, g->h };
            SDL_SetTextureColorMod(g->tex, c.r, c.g, c.b);
            SDL_RenderCopy(S.ren, g->tex, NULL, &r);
        }
        px += g->advance;
    }
    return (int)lroundf(px / S.scale);
}

static int draw_text(int font, int x, int y, Rgb c, const char *s)
{
    return draw_text_n(font, x, y, c, s, strlen(s));
}

/* Word-wrapped paragraph in width w; returns the y below it. Honours \n. */
static int draw_para(int font, int x, int y, int w, Rgb c, const char *s)
{
    int limit = (int)lroundf(w * S.scale);
    int line = (int)lroundf(S.line_h[font] / S.scale);
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s), fit = 0, i;
        /* The longest run of whole words that fits; a single word longer
         * than the line (a path) is broken anywhere. */
        for (i = 1; i <= len; i++)
            if ((i == len || s[i] == ' ') && text_width(font, s, i) <= limit)
                fit = i;
        if (fit == 0) {
            for (fit = len; fit > 1 && text_width(font, s, fit) > limit; fit--)
                ;
        }
        draw_text_n(font, x, y, c, s, fit);
        y += line;
        s += fit;
        while (*s == ' ')
            s++;
        if (s == nl)
            s++;
        else if (fit == len && !nl)
            break;
    }
    return y;
}

/* --------------------------------------------------------------- drawing */

static void fill(int x, int y, int w, int h, Rgb c)
{
    SDL_Rect r = { (int)lroundf(x * S.scale), (int)lroundf(y * S.scale),
                   (int)lroundf(w * S.scale), (int)lroundf(h * S.scale) };
    SDL_SetRenderDrawColor(S.ren, c.r, c.g, c.b, 255);
    SDL_RenderFillRect(S.ren, &r);
}

typedef struct Button {
    const char *label;
    int x, y, w, h;
    int primary;
} Button;

static int g_mx = -1, g_my = -1;

static int in_rect(int x, int y, int w, int h)
{
    return g_mx >= x && g_mx < x + w && g_my >= y && g_my < y + h;
}

/* Lay buttons out right to left along the bottom. */
static void layout_buttons(Button *b, int n)
{
    int x = WIN_W - PAD, i;
    for (i = 0; i < n; i++) {
        b[i].w = (int)lroundf(text_width(F_BOLD, b[i].label, strlen(b[i].label)) / S.scale) + 40;
        b[i].h = 38;
        x -= b[i].w;
        b[i].x = x;
        b[i].y = WIN_H - PAD - b[i].h;
        x -= 12;
    }
}

static void draw_button(const Button *b, int enabled)
{
    Rgb bg = b->primary ? C_ACCENT : (in_rect(b->x, b->y, b->w, b->h) ? C_HOVER : C_BUTTON);
    int tw = (int)lroundf(text_width(F_BOLD, b->label, strlen(b->label)) / S.scale);
    if (b->primary && in_rect(b->x, b->y, b->w, b->h))
        bg = (Rgb){ 0xf0, 0xa0, 0x3a };
    if (!enabled)
        bg = C_PANEL;
    fill(b->x, b->y, b->w, b->h, bg);
    draw_text(F_BOLD, b->x + (b->w - tw) / 2, b->y + 9,
              !enabled ? C_DIM : b->primary ? C_BG : C_TEXT, b->label);
}

static int clicked(const Button *b, const SDL_Event *e)
{
    return e->type == SDL_MOUSEBUTTONUP && e->button.button == SDL_BUTTON_LEFT &&
           in_rect(b->x, b->y, b->w, b->h);
}

static void track_mouse(const SDL_Event *e)
{
    if (e->type == SDL_MOUSEMOTION) {
        g_mx = e->motion.x;
        g_my = e->motion.y;
    } else if (e->type == SDL_MOUSEBUTTONUP || e->type == SDL_MOUSEBUTTONDOWN) {
        g_mx = e->button.x;
        g_my = e->button.y;
    }
}

/* RECOMP_SETUP_SHOT=<prefix>: the window's picture as <prefix>NNN.bmp every
 * few seconds, for seeing it in a headless compositor or a bug report. */
static void present(void)
{
    static Uint32 last;
    static int n;
    const char *prefix = getenv("RECOMP_SETUP_SHOT");
    Uint32 now = SDL_GetTicks();
    if (prefix && *prefix && (n == 0 || now - last >= 3000)) {
        int w = 0, h = 0;
        SDL_Surface *sf;
        SDL_GetRendererOutputSize(S.ren, &w, &h);
        sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (sf && SDL_RenderReadPixels(S.ren, NULL, SDL_PIXELFORMAT_ARGB8888, sf->pixels,
                                       sf->pitch) == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s%03d.bmp", prefix, n++);
            SDL_SaveBMP(sf, path);
        }
        if (sf)
            SDL_FreeSurface(sf);
        last = now;
    }
    SDL_RenderPresent(S.ren);
}

static int frame_begin(const char *heading)
{
    SDL_SetRenderDrawColor(S.ren, C_BG.r, C_BG.g, C_BG.b, 255);
    SDL_RenderClear(S.ren);
    fill(0, 0, WIN_W, 6, C_ACCENT);
    draw_text(F_HEAD, PAD, PAD - 4, C_TEXT, heading);
    return PAD + 40;
}

/* ------------------------------------------------------------ open/close */

int recomp_setup_open(const char *window_title)
{
    int dw = 0, dh = 0;
    if (S.open)
        return 1;
    snprintf(S.title, sizeof S.title, "%s", window_title ? window_title : "");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[SETUP] no display (%s); asking in the terminal\n", SDL_GetError());
        return 0;
    }
    S.win = SDL_CreateWindow(S.title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             WIN_W, WIN_H, SDL_WINDOW_ALLOW_HIGHDPI);
    if (S.win) {
        S.ren = SDL_CreateRenderer(S.win, -1, SDL_RENDERER_ACCELERATED |
                                              SDL_RENDERER_PRESENTVSYNC);
        if (!S.ren)
            S.ren = SDL_CreateRenderer(S.win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!S.win || !S.ren) {
        fprintf(stderr, "[SETUP] could not open a window (%s)\n", SDL_GetError());
        recomp_setup_close();
        return 0;
    }
    SDL_GetRendererOutputSize(S.ren, &dw, &dh);
    S.scale = dw > 0 ? (float)dw / WIN_W : 1.0f;
    if (!load_fonts()) {
        fprintf(stderr, "[SETUP] no usable font (fontconfig/FreeType)\n");
        recomp_setup_close();
        return 0;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    S.open = 1;
    return 1;
}

void recomp_setup_close(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        Glyph *g = S.glyphs[i];
        while (g) {
            Glyph *next = g->next;
            if (g->tex)
                SDL_DestroyTexture(g->tex);
            free(g);
            g = next;
        }
        S.glyphs[i] = NULL;
    }
    for (i = 0; i < F_COUNT; i++)
        if (S.face[i]) {
            FT_Done_Face(S.face[i]);
            S.face[i] = NULL;
        }
    if (S.ft) {
        FT_Done_FreeType(S.ft);
        S.ft = NULL;
    }
    if (S.ren)
        SDL_DestroyRenderer(S.ren);
    if (S.win)
        SDL_DestroyWindow(S.win);
    S.ren = NULL;
    S.win = NULL;
    /* All of SDL: the runtime starts it again on its own thread for the
     * game window (host_sdl.c). */
    if (S.open || SDL_WasInit(SDL_INIT_VIDEO))
        SDL_Quit();
    S.open = 0;
}

/* -------------------------------------------------------- the file dialog */

static struct {
    SDL_mutex *lock;
    int running, done;
    char result[4096];
} g_browse;

static int browse_thread(void *unused)
{
    const char *desk = getenv("XDG_CURRENT_DESKTOP");
    int kde = desk && strstr(desk, "KDE");
    char cmd[1024], line[4096] = "";
    FILE *p;
    const char *home = getenv("HOME");
    (void)unused;

    if (kde && system("command -v kdialog >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "kdialog --title 'Choose your game disc' --getopenfilename "
                 "'%s' 'Xbox disc image or default.xbe (*.iso *.ISO *.xiso default.xbe)' "
                 "2>/dev/null", home ? home : ".");
    else if (system("command -v zenity >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "zenity --file-selection --title='Choose your game disc' "
                 "--file-filter='Xbox disc image or default.xbe | *.iso *.ISO *.xiso default.xbe' "
                 "--file-filter='All files | *' 2>/dev/null");
    else if (system("command -v kdialog >/dev/null 2>&1") == 0)
        snprintf(cmd, sizeof cmd, "kdialog --title 'Choose your game disc' --getopenfilename "
                 "'%s' 2>/dev/null", home ? home : ".");
    else
        cmd[0] = '\0';
    if (cmd[0] && (p = popen(cmd, "r")) != NULL) {
        if (!fgets(line, sizeof line, p))
            line[0] = '\0';
        pclose(p);
        line[strcspn(line, "\r\n")] = '\0';
    } else if (!cmd[0]) {
        snprintf(line, sizeof line, "\x01");    /* no dialog program */
    }
    SDL_LockMutex(g_browse.lock);
    snprintf(g_browse.result, sizeof g_browse.result, "%s", line);
    g_browse.done = 1;
    SDL_UnlockMutex(g_browse.lock);
    return 0;
}

/* ---------------------------------------------------------- the terminal */

static int ask_terminal(const char *heading, const char *body, const char *error,
                        recomp_setup_validate_fn validate, void *ctx, char *out, size_t n)
{
    char line[4096], msg[1024];
    fprintf(stderr, "\n%s\n\n%s\n", heading, body);
    if (error && *error)
        fprintf(stderr, "\n%s\n", error);
    if (!isatty(STDIN_FILENO))
        return 0;
    for (;;) {
        fprintf(stderr, "\nPath (empty to quit): ");
        if (!fgets(line, sizeof line, stdin))
            return 0;
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0])
            return 0;
        if (validate(line, msg, sizeof msg, ctx)) {
            snprintf(out, n, "%s", line);
            return 1;
        }
        fprintf(stderr, "%s\n", msg);
    }
}

/* -------------------------------------------------------------- picking */

/* A file:// URI or a quoted path, as a paste or a drop may give it. */
static void tidy_path(char *p)
{
    size_t n;
    char *r, *w;
    while (*p == ' ' || *p == '\'' || *p == '"') memmove(p, p + 1, strlen(p));
    n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\'' || p[n - 1] == '"' ||
                 p[n - 1] == '\n' || p[n - 1] == '\r'))
        p[--n] = '\0';
    if (strncmp(p, "file://", 7) == 0) {
        memmove(p, p + 7, strlen(p + 7) + 1);
        for (r = w = p; *r; r++, w++) {        /* %20 and friends */
            if (r[0] == '%' && r[1] && r[2]) {
                char hex[3] = { r[1], r[2], 0 };
                *w = (char)strtol(hex, NULL, 16);
                r += 2;
            } else {
                *w = *r;
            }
        }
        *w = '\0';
    }
}

int recomp_setup_pick(const char *heading, const char *body, const char *error,
                      recomp_setup_validate_fn validate, void *ctx,
                      char *out, size_t out_bytes)
{
    char field[4096] = "", err[1024] = "";
    int checking = 0, quit = 0, accepted = 0;
    Button b[3] = { { "Continue", 0, 0, 0, 0, 1 }, { "Browse...", 0, 0, 0, 0, 0 },
                    { "Quit", 0, 0, 0, 0, 0 } };

    if (!S.open)
        return ask_terminal(heading, body, error, validate, ctx, out, out_bytes);
    if (error)
        snprintf(err, sizeof err, "%s", error);
    if (!g_browse.lock)
        g_browse.lock = SDL_CreateMutex();
    layout_buttons(b, 3);
    SDL_StartTextInput();

    while (!quit && !accepted) {
        SDL_Event e;
        int y, field_y, browsing;
        SDL_LockMutex(g_browse.lock);
        browsing = g_browse.running;
        if (g_browse.running && g_browse.done) {
            g_browse.running = 0;
            if (g_browse.result[0] == '\x01')
                snprintf(err, sizeof err, "No file dialog program here (zenity or kdialog); "
                         "type or paste the path instead, or drop the file on this window.");
            else if (g_browse.result[0]) {
                snprintf(field, sizeof field, "%s", g_browse.result);
                checking = 1;
            }
        }
        SDL_UnlockMutex(g_browse.lock);

        while (SDL_PollEvent(&e)) {
            track_mouse(&e);
            if (e.type == SDL_QUIT) {
                quit = 1;
            } else if (e.type == SDL_DROPFILE) {
                snprintf(field, sizeof field, "%s", e.drop.file);
                SDL_free(e.drop.file);
                checking = 1;
            } else if (e.type == SDL_TEXTINPUT && !browsing) {
                size_t n = strlen(field);
                snprintf(field + n, sizeof field - n, "%s", e.text.text);
                err[0] = '\0';
            } else if (e.type == SDL_KEYDOWN && !browsing) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) {
                    quit = 1;
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    checking = field[0] != '\0';
                } else if (k == SDLK_BACKSPACE && field[0]) {
                    size_t n = strlen(field);
                    while (n > 0 && ((unsigned char)field[n - 1] & 0xC0) == 0x80)
                        n--;
                    field[n > 0 ? n - 1 : 0] = '\0';
                    err[0] = '\0';
                } else if (k == SDLK_v && (e.key.keysym.mod & KMOD_CTRL)) {
                    char *clip = SDL_GetClipboardText();
                    if (clip) {
                        size_t n = strlen(field);
                        snprintf(field + n, sizeof field - n, "%s", clip);
                        SDL_free(clip);
                    }
                } else if (k == SDLK_u && (e.key.keysym.mod & KMOD_CTRL)) {
                    field[0] = '\0';
                }
            } else if (!browsing && clicked(&b[0], &e)) {
                checking = field[0] != '\0';
                if (!checking)
                    snprintf(err, sizeof err, "Choose the disc image or folder first.");
            } else if (!browsing && clicked(&b[1], &e)) {
                SDL_LockMutex(g_browse.lock);
                g_browse.running = 1;
                g_browse.done = 0;
                SDL_UnlockMutex(g_browse.lock);
                SDL_DetachThread(SDL_CreateThread(browse_thread, "browse", NULL));
            } else if (clicked(&b[2], &e)) {
                quit = 1;
            }
        }

        y = frame_begin(heading);
        y = draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_TEXT, body) + 18;
        field_y = y;
        fill(PAD, field_y, WIN_W - 2 * PAD, 40, C_FIELD);
        fill(PAD, field_y + 38, WIN_W - 2 * PAD, 2, field[0] ? C_ACCENT : C_BUTTON);
        {
            /* Show the end of a long path, which is the part that differs. */
            const char *shown = field;
            int limit = (int)lroundf((WIN_W - 2 * PAD - 24) * S.scale);
            while (*shown && text_width(F_BODY, shown, strlen(shown)) > limit)
                utf8_next(&shown);
            if (field[0]) {
                int end = draw_text(F_BODY, PAD + 12, field_y + 10, C_TEXT, shown);
                if ((SDL_GetTicks() / 500) % 2 == 0)
                    fill(end + 1, field_y + 10, 2, 20, C_TEXT);
            } else {
                draw_text(F_BODY, PAD + 12, field_y + 10, C_DIM,
                          "Type or paste a path, drop a file here, or Browse...");
            }
        }
        y = field_y + 56;
        if (browsing)
            draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_DIM, "Waiting for the file dialog...");
        else if (checking)
            draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_DIM, "Checking...");
        else if (err[0])
            draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_ERROR, err);
        draw_button(&b[0], !browsing);
        draw_button(&b[1], !browsing);
        draw_button(&b[2], 1);
        present();

        if (checking) {
            char msg[1024];
            tidy_path(field);
            checking = 0;
            if (validate(field, msg, sizeof msg, ctx)) {
                snprintf(out, out_bytes, "%s", field);
                accepted = 1;
            } else {
                snprintf(err, sizeof err, "%s", msg);
            }
        }
        SDL_Delay(16);
    }
    SDL_StopTextInput();
    return accepted;
}

/* --------------------------------------------------------------- running */

typedef struct Child {
    pid_t pid;
    int fd;
    char buf[8192];
    size_t len;
    FILE *log;
} Child;

static void mkdir_parents(const char *path)
{
    char tmp[4096], *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
}

static int child_start(Child *c, char *const argv[], const char *log_path)
{
    int fds[2];
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int rc;
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (log_path)
        mkdir_parents(log_path);
    c->log = log_path ? fopen(log_path, "w") : NULL;
    if (pipe2(fds, O_CLOEXEC) != 0)
        return 0;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_init(&at);
    /* Its own process group, so Cancel stops the compilers it started too. */
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    rc = posix_spawnp(&c->pid, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        if (c->log) {
            fprintf(c->log, "could not start %s: %s\n", argv[0], strerror(rc));
            fclose(c->log);
            c->log = NULL;
        }
        errno = rc;
        return 0;
    }
    c->fd = fds[0];
    fcntl(c->fd, F_SETFL, O_NONBLOCK);
    return 1;
}

/* Complete lines read so far, one per call; NULL when none is waiting. The
 * returned line lives until the next call. */
static const char *child_line(Child *c, int *eof)
{
    static char line[sizeof c->buf];
    char *nl;
    *eof = 0;
    for (;;) {
        nl = memchr(c->buf, '\n', c->len);
        if (nl) {
            size_t n = (size_t)(nl - c->buf);
            memcpy(line, c->buf, n);
            line[n] = '\0';
            memmove(c->buf, nl + 1, c->len - n - 1);
            c->len -= n + 1;
            if (c->log)
                fprintf(c->log, "%s\n", line);
            return line;
        }
        if (c->len == sizeof c->buf) {          /* an overlong line: cut it */
            memcpy(line, c->buf, c->len - 1);
            line[c->len - 1] = '\0';
            c->len = 0;
            return line;
        }
        {
            ssize_t r = read(c->fd, c->buf + c->len, sizeof c->buf - c->len);
            if (r > 0) {
                c->len += (size_t)r;
                continue;
            }
            if (r == 0)
                *eof = 1;
            return NULL;
        }
    }
}

static int child_finish(Child *c)
{
    int status = 0;
    if (c->fd >= 0)
        close(c->fd);
    waitpid(c->pid, &status, 0);
    if (c->log)
        fclose(c->log);
    c->fd = -1;
    c->log = NULL;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void open_file(const char *path)
{
    char *argv[] = { "xdg-open", (char *)path, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ) == 0)
        waitpid(pid, NULL, WNOHANG);
}

static int run_terminal(char *const argv[], const char *log_path)
{
    Child c;
    int eof, rc;
    const char *line;
    if (!child_start(&c, argv, log_path)) {
        fprintf(stderr, "could not start %s: %s\n", argv[0], strerror(errno));
        return 0;
    }
    fcntl(c.fd, F_SETFL, 0);                    /* blocking is fine here */
    for (;;) {
        line = child_line(&c, &eof);
        if (line) {
            if (strncmp(line, "@progress ", 10) == 0) {
                int lo, hi, off = 0;
                if (sscanf(line + 10, "%d %d %n", &lo, &hi, &off) >= 2)
                    fprintf(stderr, "[%3d%%] %s\n", lo, line + 10 + off);
            } else {
                fprintf(stderr, "%s\n", line);
            }
        } else if (eof) {
            break;
        }
    }
    rc = child_finish(&c);
    return rc == 0;
}

int recomp_setup_run(const char *heading, const char *body, char *const argv[],
                     const char *log_path)
{
    if (!S.open)
        return run_terminal(argv, log_path);
    for (;;) {
        Child c;
        int started, finished = 0, failed = 0, cancel = 0, retry = 0, eof = 0;
        float lo = 0, hi = 2, shown = 0;
        Uint32 t0 = SDL_GetTicks(), stage_t = t0;
        char what[256] = "Starting", error[1024] = "", tail[6][256];
        int tail_n = 0, i;
        Button run_b[1] = { { "Cancel", 0, 0, 0, 0, 0 } };
        Button fail_b[3] = { { "Try again", 0, 0, 0, 0, 1 }, { "Open log", 0, 0, 0, 0, 0 },
                             { "Quit", 0, 0, 0, 0, 0 } };

        memset(tail, 0, sizeof tail);
        layout_buttons(run_b, 1);
        layout_buttons(fail_b, 3);
        started = child_start(&c, argv, log_path);
        if (!started) {
            snprintf(error, sizeof error, "Could not start %s (%s). It needs to be "
                     "installed for the first-time build.", argv[0], strerror(errno));
            failed = 1;
        }

        while (!retry) {
            SDL_Event e;
            const char *line;
            int y;
            Uint32 now = SDL_GetTicks();

            while (started && !finished && (line = child_line(&c, &eof)) != NULL) {
                if (strncmp(line, "@progress ", 10) == 0) {
                    int a, b2, off = 0;
                    if (sscanf(line + 10, "%d %d %n", &a, &b2, &off) >= 2) {
                        lo = (float)a;
                        hi = (float)b2;
                        if (shown < lo) shown = lo;
                        stage_t = now;
                        snprintf(what, sizeof what, "%s", line + 10 + off);
                    }
                } else if (line[0]) {
                    if (strncmp(line, "error: ", 7) == 0)
                        snprintf(error, sizeof error, "%s", line + 7);
                    if (tail_n == 6) {
                        memmove(tail[0], tail[1], sizeof tail[0] * 5);
                        tail_n = 5;
                    }
                    snprintf(tail[tail_n++], sizeof tail[0], "%s", line);
                }
            }
            if (started && !finished && eof) {
                int rc = child_finish(&c);
                finished = 1;
                if (rc == 0)
                    return 1;
                failed = 1;
                if (!error[0])
                    snprintf(error, sizeof error, "The build stopped (exit status %d).", rc);
            }

            while (SDL_PollEvent(&e)) {
                track_mouse(&e);
                if (e.type == SDL_QUIT) {
                    /* Also what SIGINT/SIGTERM become under SDL. */
                    fprintf(stderr, "[SETUP] build stopped: the window was closed or the process was signalled\n");
                    cancel = 1;
                } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    fprintf(stderr, "[SETUP] build stopped: Escape\n");
                    cancel = 1;
                } else if (!failed && clicked(&run_b[0], &e)) {
                    fprintf(stderr, "[SETUP] build stopped: Cancel\n");
                    cancel = 1;
                } else if (failed && (clicked(&fail_b[0], &e) ||
                                      (e.type == SDL_KEYDOWN &&
                                       e.key.keysym.sym == SDLK_RETURN))) {
                    retry = 1;
                } else if (failed && clicked(&fail_b[1], &e) && log_path) {
                    open_file(log_path);
                } else if (failed && clicked(&fail_b[2], &e)) {
                    cancel = 1;
                }
            }
            if (cancel) {
                if (started && !finished) {
                    kill(-c.pid, SIGTERM);
                    child_finish(&c);
                }
                return 0;
            }

            /* The bar eases from the step's start towards its end, never
             * quite getting there until the next step says so. */
            if (!failed) {
                float span = hi - lo, tau = span * 2.2f + 1.0f;
                float t = (now - stage_t) / 1000.0f;
                float target = lo + span * (1.0f - expf(-t / tau));
                if (target > shown) shown = target;
            }

            y = frame_begin(heading);
            y = draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_TEXT, body) + 20;
            fill(PAD, y, WIN_W - 2 * PAD, 14, C_FIELD);
            fill(PAD, y, (int)((WIN_W - 2 * PAD) * (shown / 100.0f)), 14,
                 failed ? C_ERROR : C_ACCENT);
            y += 24;
            {
                char line2[320];
                unsigned secs = (now - t0) / 1000;
                snprintf(line2, sizeof line2, "%s", failed ? "Stopped" : what);
                draw_text(F_BOLD, PAD, y, C_TEXT, line2);
                snprintf(line2, sizeof line2, "%u:%02u", secs / 60, secs % 60);
                draw_text(F_BODY, WIN_W - PAD - 50, y, C_DIM, line2);
            }
            y += 32;
            if (failed) {
                y = draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_ERROR, error) + 6;
                if (log_path) {
                    char where[1200];
                    snprintf(where, sizeof where, "Full log: %s", log_path);
                    draw_para(F_SMALL, PAD, y, WIN_W - 2 * PAD, C_DIM, where);
                }
                for (i = 0; i < 3; i++)
                    draw_button(&fail_b[i], 1);
            } else {
                for (i = 0; i < tail_n; i++)
                    y = draw_para(F_SMALL, PAD, y, WIN_W - 2 * PAD, C_DIM, tail[i]);
                draw_button(&run_b[0], 1);
            }
            present();
            SDL_Delay(30);
        }
    }
}

int recomp_setup_capture(char *const argv[], char *msg, size_t msg_bytes)
{
    Child c;
    int eof, rc;
    const char *line;
    char last[1024] = "", err[1024] = "";
    if (!child_start(&c, argv, NULL)) {
        snprintf(msg, msg_bytes, "Could not start %s (%s).", argv[0], strerror(errno));
        return -1;
    }
    fcntl(c.fd, F_SETFL, 0);
    for (;;) {
        line = child_line(&c, &eof);
        if (line) {
            if (strncmp(line, "error: ", 7) == 0)
                snprintf(err, sizeof err, "%s", line + 7);
            else if (line[0])
                snprintf(last, sizeof last, "%s", line);
        } else if (eof) {
            break;
        }
    }
    rc = child_finish(&c);
    snprintf(msg, msg_bytes, "%s", err[0] ? err : last);
    return rc;
}

/* --------------------------------------------------------------- message */

void recomp_setup_message(const char *heading, const char *body)
{
    Button ok[1] = { { "OK", 0, 0, 0, 0, 1 } };
    int done = 0;
    if (!S.open) {
        fprintf(stderr, "\n%s\n\n%s\n", heading, body);
        return;
    }
    layout_buttons(ok, 1);
    while (!done) {
        SDL_Event e;
        int y;
        while (SDL_PollEvent(&e)) {
            track_mouse(&e);
            if (e.type == SDL_QUIT || clicked(&ok[0], &e) ||
                (e.type == SDL_KEYDOWN && (e.key.keysym.sym == SDLK_RETURN ||
                                           e.key.keysym.sym == SDLK_ESCAPE)))
                done = 1;
        }
        y = frame_begin(heading);
        draw_para(F_BODY, PAD, y, WIN_W - 2 * PAD, C_TEXT, body);
        draw_button(&ok[0], 1);
        present();
        SDL_Delay(30);
    }
}
