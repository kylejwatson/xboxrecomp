/*
 * recomp_mouse.c -- see recomp_mouse.h.
 *
 * Two threads meet here. The game window's thread receives WM_INPUT and adds
 * each movement to a pair of interlocked running totals, and sets or clears a
 * bit per button; it never waits for anything, because a window thread that
 * blocks stalls the Present that is waiting on it. The title's look hook, on
 * a guest thread, takes what the totals gained once a game frame -- up to
 * the last frame mark (recomp_mouse_frame_mark, at each Swap), not up to the
 * moment it runs, so a frame's turn covers the same span of time as the rest
 * of the frame even when the host was late to present the one before.
 *
 * An earlier version (e00a4eb, never merged here) turned the movement into a
 * right-stick deflection. That cannot give 1:1 aim: the title's stick code
 * puts a deadzone, a response curve and an acceleration ramp between the hand
 * and the camera, and a mouse that stops still decays through them. The raw
 * input and the cursor capture below are that version's; the stick model is
 * gone, replaced by recomp_mouse_take_motion for a title-side hook.
 *
 * Capture. While mouse look is active and the window is in front, the cursor
 * is clipped to the picture and hidden, because a cursor that wanders off the
 * window takes the input focus with it at the first click. F8 lets it go
 * (F9, F10 and F11 are the frame-rate overlay, the frame cap and the frame
 * capture; F12 breaks into an attached debugger); a click on the picture,
 * or coming back to the window, takes it again. The click that does so is
 * not passed to the title.
 */
#include "recomp_mouse.h"
#include "input_bindings.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
typedef volatile LONG mouse_atomic;
#  define ATOMIC_ADD(p, v)     InterlockedExchangeAdd((p), (v))
#  define ATOMIC_XCHG(p, v)    InterlockedExchange((p), (v))
#  define ATOMIC_CAS(p, n, o)  InterlockedCompareExchange((p), (n), (o))
#  define ATOMIC_OR(p, v)      InterlockedOr((p), (v))
#  define ATOMIC_AND(p, v)     InterlockedAnd((p), (v))
static SRWLOCK g_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&g_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#  include <pthread.h>
#  include <time.h>
typedef volatile long mouse_atomic;
#  define ATOMIC_ADD(p, v)     __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_XCHG(p, v)    __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_CAS(p, n, o)  __sync_val_compare_and_swap((p), (o), (n))
#  define ATOMIC_OR(p, v)      __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#  define ATOMIC_AND(p, v)     __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&g_lock)
#  define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* Counts older than this when the title next takes them are dropped: the
 * hook was not running (paused, a cutscene, a menu), and the camera should
 * not jump by everything the hand did meanwhile. Several game frames long, so
 * a slow frame never loses real movement. */
#define MOUSE_STALE_SECONDS 0.25
/* A wheel notch is held this long, then let go for MOUSE_WHEEL_GAP before
 * the next one: long enough for a title polling once a frame to see both. */
#define MOUSE_WHEEL_PRESS 0.060
#define MOUSE_WHEEL_GAP   0.040
#define MOUSE_WHEEL_QUEUE 4
/* The key that lets the cursor go. */
#define MOUSE_RELEASE_VK  0x77          /* VK_F8 */

static RecompMouseConfig g_cfg = { 1, 2.5, 0, 0 };
static int g_title_look;

/* Every count fed since start-up. They only ever grow (or wrap), so a span
 * of time is the difference of two readings: see mouse_delta. */
static mouse_atomic g_tot_x, g_tot_y;
static mouse_atomic g_buttons;          /* bit per RECOMP_MOUSE_LEFT..X2 */
static mouse_atomic g_wheel_pending[2]; /* notches not yet pressed: up, down */
static mouse_atomic g_captured;
static mouse_atomic g_recording, g_rec_x, g_rec_y;

/* Under g_lock. */
static double g_last_take;
static struct { int pressed; double until; } g_wheel[2];
/* The totals when the title last took motion, and at the last frame mark.
 * g_mark_seq counts marks; g_taken_seq is the mark the last take used, so
 * a take with no mark since the one before takes up to now. */
static long g_taken_x, g_taken_y, g_mark_x, g_mark_y;
static unsigned long g_mark_seq, g_taken_seq;

/* b - a for two readings of a running total, through a wrap: the totals are
 * 32 bits on Windows. */
static long mouse_delta(long b, long a)
{
    return (long)(int32_t)((uint32_t)b - (uint32_t)a);
}

/* Counts fed so far are not the title's: the cursor was just captured or
 * let go, or the hook stopped running long enough for them to be stale. */
static void drop_pending_locked(void)
{
    g_taken_x = g_mark_x = g_tot_x;
    g_taken_y = g_mark_y = g_tot_y;
    g_taken_seq = g_mark_seq;
}

static void drop_pending(void)
{
    LOCK();
    drop_pending_locked();
    UNLOCK();
}

static double now_seconds(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

void recomp_mouse_configure(const RecompMouseConfig *cfg)
{
    if (!cfg)
        return;
    g_cfg = *cfg;
    if (!(g_cfg.sensitivity > 0.0))
        g_cfg.sensitivity = 2.5;
}

const RecompMouseConfig *recomp_mouse_config(void)
{
    recomp_bindings_init();
    return &g_cfg;
}

void recomp_mouse_title_has_look(void)
{
    g_title_look = 1;
}

int recomp_mouse_look_active(void)
{
    recomp_bindings_init();
    return g_title_look && g_cfg.look;
}

int recomp_mouse_wanted(void)
{
    return recomp_mouse_look_active() || g_cfg.buttons_bound;
}

int recomp_mouse_captured(void)
{
    return g_captured != 0;
}

void recomp_mouse_feed_motion(long dx, long dy)
{
    if (dx)
        ATOMIC_ADD(&g_tot_x, dx);
    if (dy)
        ATOMIC_ADD(&g_tot_y, dy);
    if (g_recording) {
        if (dx)
            ATOMIC_ADD(&g_rec_x, dx);
        if (dy)
            ATOMIC_ADD(&g_rec_y, dy);
    }
}

void recomp_mouse_record_motion(int on)
{
    ATOMIC_XCHG(&g_recording, on ? 1 : 0);
}

int recomp_mouse_take_recorded(long *dx, long *dy)
{
    long x = ATOMIC_XCHG(&g_rec_x, 0), y = ATOMIC_XCHG(&g_rec_y, 0);

    if (dx) *dx = x;
    if (dy) *dy = y;
    return x != 0 || y != 0;
}

void recomp_mouse_frame_mark(void)
{
    LOCK();
    g_mark_x = g_tot_x;
    g_mark_y = g_tot_y;
    g_mark_seq++;
    UNLOCK();
}

int recomp_mouse_take_motion(long *dx, long *dy)
{
    double now;
    long x = 0, y = 0, to_x, to_y;

    LOCK();
    now = now_seconds();
    if (g_last_take <= 0.0 || now - g_last_take > MOUSE_STALE_SECONDS) {
        drop_pending_locked();
    } else {
        if (g_taken_seq != g_mark_seq) {
            to_x = g_mark_x;            /* up to the frame mark */
            to_y = g_mark_y;
        } else {
            to_x = g_tot_x;             /* no mark since the last take */
            to_y = g_tot_y;
        }
        x = mouse_delta(to_x, g_taken_x);
        y = mouse_delta(to_y, g_taken_y);
        g_taken_x = to_x;
        g_taken_y = to_y;
        g_taken_seq = g_mark_seq;
    }
    g_last_take = now;
    UNLOCK();
    if (g_cfg.invert_y)
        y = -y;
    if (dx) *dx = x;
    if (dy) *dy = y;
    return x != 0 || y != 0;
}

double recomp_mouse_counts_to_degrees(long counts)
{
    return (double)counts * RECOMP_MOUSE_DEGREES_PER_COUNT * g_cfg.sensitivity;
}

/* One wheel direction, stepped forward to `now`: release a press that has
 * been held long enough, start the next queued notch once the gap is over. */
static int wheel_read(int dir, double now)
{
    if (g_wheel[dir].pressed && now >= g_wheel[dir].until) {
        g_wheel[dir].pressed = 0;
        g_wheel[dir].until = now + MOUSE_WHEEL_GAP;
    }
    if (!g_wheel[dir].pressed && now >= g_wheel[dir].until) {
        long n = g_wheel_pending[dir];

        if (n > 0 && ATOMIC_CAS(&g_wheel_pending[dir], n - 1, n) == n) {
            g_wheel[dir].pressed = 1;
            g_wheel[dir].until = now + MOUSE_WHEEL_PRESS;
        }
    }
    return g_wheel[dir].pressed;
}

int recomp_mouse_source(int which)
{
    int on;

    if (which >= RECOMP_MOUSE_LEFT && which <= RECOMP_MOUSE_X2)
        return (g_buttons & (1L << which)) ? 255 : 0;
    if (which != RECOMP_MOUSE_WHEEL_UP && which != RECOMP_MOUSE_WHEEL_DOWN)
        return 0;
    LOCK();
    on = wheel_read(which - RECOMP_MOUSE_WHEEL_UP, now_seconds());
    UNLOCK();
    return on ? 255 : 0;
}

/* ---- the window -------------------------------------------------------- */

#if defined(_WIN32)

static HWND g_hwnd;
static int  g_attached;
static long g_suppress;                 /* window thread only */

static void wheel_notch(int dir, int notches)
{
    long n;

    if (notches < 1)
        notches = 1;
    n = ATOMIC_ADD(&g_wheel_pending[dir], notches) + notches;
    if (n > MOUSE_WHEEL_QUEUE)
        ATOMIC_XCHG(&g_wheel_pending[dir], MOUSE_WHEEL_QUEUE);
}

/* Physical button bits (raw input and GetAsyncKeyState both report the
 * physical buttons) as the logical ones the sources name. */
static long logical_buttons(long physical)
{
    if (GetSystemMetrics(SM_SWAPBUTTON)) {
        long l = physical & (1L << RECOMP_MOUSE_LEFT), r = physical & (1L << RECOMP_MOUSE_RIGHT);

        physical &= ~((1L << RECOMP_MOUSE_LEFT) | (1L << RECOMP_MOUSE_RIGHT));
        if (l) physical |= 1L << RECOMP_MOUSE_RIGHT;
        if (r) physical |= 1L << RECOMP_MOUSE_LEFT;
    }
    return physical;
}

static int cursor_in_client(HWND hwnd)
{
    POINT pt;
    RECT rc;

    if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc))
        return 0;
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

static RECT g_pin;                      /* where the cursor is held */

/* The cursor is pinned to one pixel at the middle of the picture, not just
 * kept inside the window: raw input still reports every count, and a turn
 * that runs on can never leave the cursor at an edge, where a click (or a
 * clip the system dropped, or a DPI-scaled rectangle) lands on a neighbour. */
static void clip_to_client(HWND hwnd)
{
    RECT rc;
    LONG cx, cy;

    GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, NULL, (POINT *)&rc, 2);
    cx = (rc.left + rc.right) / 2;
    cy = (rc.top + rc.bottom) / 2;
    g_pin.left = cx;
    g_pin.top = cy;
    g_pin.right = cx + 1;
    g_pin.bottom = cy + 1;
    ClipCursor(&g_pin);
    SetCursorPos(cx, cy);
    /* Clicks go to this window whatever is under the cursor. */
    if (GetCapture() != hwnd)
        SetCapture(hwnd);
}

/* Something else (another program's ClipCursor, a desktop switch, a dialog)
 * may have undone the pin without telling the window. */
static void recheck_pin(HWND hwnd)
{
    static DWORD last;
    DWORD now = GetTickCount();
    RECT cur;

    if (now - last < 200)
        return;
    last = now;
    if (GetForegroundWindow() != hwnd)
        return;
    if (!GetClipCursor(&cur) || cur.left != g_pin.left || cur.top != g_pin.top ||
        cur.right != g_pin.right || cur.bottom != g_pin.bottom ||
        GetCapture() != hwnd)
        clip_to_client(hwnd);
}

/* Whatever is held now is the click that brought the window back, and the
 * title should not see it; its release clears it. */
static void suppress_held(void)
{
    long held = 0;

    if (GetAsyncKeyState(VK_LBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_LEFT;
    if (GetAsyncKeyState(VK_RBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_RIGHT;
    if (GetAsyncKeyState(VK_MBUTTON) & 0x8000)  held |= 1L << RECOMP_MOUSE_MIDDLE;
    if (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) held |= 1L << RECOMP_MOUSE_X1;
    if (GetAsyncKeyState(VK_XBUTTON2) & 0x8000) held |= 1L << RECOMP_MOUSE_X2;
    g_suppress |= logical_buttons(held);
}

static void capture(HWND hwnd)
{
    static int said;

    if (!recomp_mouse_look_active() || g_captured)
        return;
    if (GetForegroundWindow() != hwnd || IsIconic(hwnd))
        return;
    clip_to_client(hwnd);
    SetCursor(NULL);
    drop_pending();
    suppress_held();
    ATOMIC_XCHG(&g_captured, 1);
    if (!said) {
        said = 1;
        fprintf(stderr, "[INPUT] mouse captured: F8 lets the cursor go, a click "
                        "on the picture takes it back\n");
        fflush(stderr);
    }
}

static void release(void)
{
    if (!g_captured)
        return;
    ATOMIC_XCHG(&g_captured, 0);
    ClipCursor(NULL);
    if (g_hwnd && GetCapture() == g_hwnd)
        ReleaseCapture();
    drop_pending();
}

/* Everything let go: the window lost the focus, and the button-up messages
 * that would have cleared these now go to someone else. */
static void drop_buttons(void)
{
    ATOMIC_XCHG(&g_buttons, 0);
    g_suppress = 0;
}

static void raw_mouse(HWND hwnd, const RAWMOUSE *m)
{
    static const USHORT down[5] = {
        RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
        RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN
    };
    static const USHORT up[5] = {
        RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
        RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP
    };
    static LONG abs_x, abs_y;
    static int have_abs;
    const int look = recomp_mouse_look_active();
    USHORT f = m->usButtonFlags;
    long dx, dy, went_down = 0, went_up = 0;
    int i;

    /* Remote desktop and some virtual machines report where the pointer
     * is, 0..65535 across the screen, rather than how far it moved. */
    if (m->usFlags & MOUSE_MOVE_ABSOLUTE) {
        int virt = (m->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        int w = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        int h = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);

        dx = have_abs ? (long)((double)(m->lLastX - abs_x) * w / 65535.0) : 0;
        dy = have_abs ? (long)((double)(m->lLastY - abs_y) * h / 65535.0) : 0;
        abs_x = m->lLastX;
        abs_y = m->lLastY;
        have_abs = 1;
    } else {
        dx = m->lLastX;
        dy = m->lLastY;
    }
    if (look && g_captured) {
        recomp_mouse_feed_motion(dx, dy);
        recheck_pin(hwnd);
    }

    for (i = 0; i < 5; i++) {
        if (f & down[i]) went_down |= 1L << i;
        if (f & up[i])   went_up   |= 1L << i;
    }
    went_down = logical_buttons(went_down);
    went_up = logical_buttons(went_up);

    if (went_up) {
        ATOMIC_AND(&g_buttons, ~went_up);
        g_suppress &= ~went_up;
    }
    if (went_down) {
        if (look && !g_captured) {
            /* A click on the picture takes the mouse back, and is only
             * that: the title does not see it. */
            if (cursor_in_client(hwnd)) {
                capture(hwnd);
                g_suppress |= went_down;
            }
            went_down = 0;
        } else if (!look && !cursor_in_client(hwnd)) {
            went_down = 0;              /* the title bar, a border */
        }
        went_down &= ~g_suppress;
        if (went_down)
            ATOMIC_OR(&g_buttons, went_down);
    }

    if (f & RI_MOUSE_WHEEL) {
        SHORT delta = (SHORT)m->usButtonData;

        if (delta && (look ? g_captured != 0 : cursor_in_client(hwnd)))
            wheel_notch(delta > 0 ? 0 : 1, (delta < 0 ? -delta : delta) / WHEEL_DELTA);
    }
}

void recomp_mouse_attach(void *hwnd_ptr)
{
    HWND hwnd = (HWND)hwnd_ptr;
    RAWINPUTDEVICE rid;

    if (!hwnd || g_attached || !recomp_mouse_wanted())
        return;
    /* Usage page 1 (generic desktop), usage 2 (mouse). No RIDEV_INPUTSINK:
     * input arrives only while this window is in front, which is the rule
     * a player expects. Not RIDEV_NOLEGACY either: the window still gets
     * its ordinary mouse messages, for the title bar and the frame. */
    memset(&rid, 0, sizeof rid);
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        fprintf(stderr, "[INPUT] mouse: RegisterRawInputDevices failed (%lu); "
                        "the mouse will do nothing\n", GetLastError());
        fflush(stderr);
        return;
    }
    g_hwnd = hwnd;
    g_attached = 1;
    if (recomp_mouse_look_active())
        fprintf(stderr, "[INPUT] mouse look: %.3f degrees per count (sensitivity %.2f), "
                        "%s%s; click the picture to capture, F8 to release\n",
                RECOMP_MOUSE_DEGREES_PER_COUNT * g_cfg.sensitivity, g_cfg.sensitivity,
                g_cfg.invert_y ? "Y inverted" : "Y not inverted",
                g_cfg.buttons_bound ? "; mouse buttons are bound" : "");
    else
        fprintf(stderr, "[INPUT] mouse buttons are bound; read from the game window\n");
    fflush(stderr);
    /* The window is created visible, so its first activation has already
     * gone by without us: take the cursor now if it came up in front. */
    capture(hwnd);
}

int recomp_mouse_window_message(void *hwnd_ptr, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result)
{
    HWND hwnd = (HWND)hwnd_ptr;

    if (!g_attached || hwnd != g_hwnd)
        return 0;
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;

        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE)
            raw_mouse(hwnd, &ri.data.mouse);
        return 0;                       /* DefWindowProc frees the input */
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            release();
            drop_buttons();
        } else if (!HIWORD(wp)) {
            if (LOWORD(wp) == WA_CLICKACTIVE)
                suppress_held();        /* a click to focus is not a shot */
            /* Back by Alt+Tab, or by a click on the picture. A click on the
             * title bar is someone moving the window: leave the cursor be. */
            if (LOWORD(wp) == WA_ACTIVE || cursor_in_client(hwnd))
                capture(hwnd);
        }
        return 0;
    case WM_KILLFOCUS:
        release();
        drop_buttons();
        return 0;
    case WM_SIZE:
    case WM_MOVE:
        if (g_captured)
            clip_to_client(hwnd);       /* Alt+Enter, or the window moved */
        return 0;
    case WM_SETCURSOR:
        if (g_captured && LOWORD(lp) == HTCLIENT) {
            SetCursor(NULL);
            if (result)
                *result = TRUE;
            return 1;
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == MOUSE_RELEASE_VK && !(lp & (1 << 30)) && recomp_mouse_look_active()) {
            if (g_captured)
                release();
            else
                capture(hwnd);
            if (result)
                *result = 0;
            return 1;
        }
        return 0;
    case WM_DESTROY:
        release();
        return 0;
    default:
        return 0;
    }
}

#else   /* the SDL window (src/platform/host_sdl.c) */

/* The same rules as the raw-input path above, on SDL: relative mouse mode is
 * the capture (SDL hides the cursor, keeps it in the window and reports raw
 * motion -- XInput2 on X11, the relative-pointer protocol on Wayland), a
 * click on the picture takes the mouse, F8 lets it go, losing the focus lets
 * it go and drops every button. Everything here runs on SDL's event thread,
 * which is the thread allowed to change the relative mode. */
#include <SDL.h>
#include "platform/host_sdl.h"

static SDL_Window *g_win;
static int  g_attached;
static long g_suppress;                 /* event thread only */

static void wheel_notch(int dir, int notches)
{
    long n;

    if (notches < 1)
        notches = 1;
    n = ATOMIC_ADD(&g_wheel_pending[dir], notches) + notches;
    if (n > MOUSE_WHEEL_QUEUE)
        ATOMIC_XCHG(&g_wheel_pending[dir], MOUSE_WHEEL_QUEUE);
}

static long button_bit(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT:   return 1L << RECOMP_MOUSE_LEFT;
    case SDL_BUTTON_RIGHT:  return 1L << RECOMP_MOUSE_RIGHT;
    case SDL_BUTTON_MIDDLE: return 1L << RECOMP_MOUSE_MIDDLE;
    case SDL_BUTTON_X1:     return 1L << RECOMP_MOUSE_X1;
    case SDL_BUTTON_X2:     return 1L << RECOMP_MOUSE_X2;
    default:                return 0;
    }
}

static long held_buttons(void)
{
    Uint32 st = SDL_GetMouseState(NULL, NULL);
    long held = 0;

    if (st & SDL_BUTTON(SDL_BUTTON_LEFT))   held |= 1L << RECOMP_MOUSE_LEFT;
    if (st & SDL_BUTTON(SDL_BUTTON_RIGHT))  held |= 1L << RECOMP_MOUSE_RIGHT;
    if (st & SDL_BUTTON(SDL_BUTTON_MIDDLE)) held |= 1L << RECOMP_MOUSE_MIDDLE;
    if (st & SDL_BUTTON(SDL_BUTTON_X1))     held |= 1L << RECOMP_MOUSE_X1;
    if (st & SDL_BUTTON(SDL_BUTTON_X2))     held |= 1L << RECOMP_MOUSE_X2;
    return held;
}

static void capture(void)
{
    static int said;

    if (!g_win || !recomp_mouse_look_active() || g_captured)
        return;
    if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS))
        return;
    if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
        fprintf(stderr, "[INPUT] mouse: relative mode unavailable (%s)\n", SDL_GetError());
        fflush(stderr);
        return;
    }
    drop_pending();
    g_suppress |= held_buttons();       /* the click that captured is not a shot */
    ATOMIC_XCHG(&g_captured, 1);
    if (!said) {
        said = 1;
        fprintf(stderr, "[INPUT] mouse captured: F8 lets the cursor go, a click "
                        "on the picture takes it back\n");
        fflush(stderr);
    }
}

static void release(void)
{
    if (!g_captured)
        return;
    ATOMIC_XCHG(&g_captured, 0);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    drop_pending();
}

static void drop_buttons(void)
{
    ATOMIC_XCHG(&g_buttons, 0);
    g_suppress = 0;
}

static int sdl_event(const void *ev)
{
    const SDL_Event *e = (const SDL_Event *)ev;
    const int look = recomp_mouse_look_active();
    long bit;

    switch (e->type) {
    case SDL_MOUSEMOTION:
        if (look && g_captured)
            recomp_mouse_feed_motion(e->motion.xrel, e->motion.yrel);
        return 0;
    case SDL_MOUSEBUTTONUP:
        bit = button_bit(e->button.button);
        ATOMIC_AND(&g_buttons, ~bit);
        g_suppress &= ~bit;
        return 0;
    case SDL_MOUSEBUTTONDOWN:
        bit = button_bit(e->button.button);
        if (look && !g_captured) {
            /* A click on the picture takes the mouse back, and is only that. */
            capture();
            g_suppress |= bit;
            return 0;
        }
        bit &= ~g_suppress;
        if (bit)
            ATOMIC_OR(&g_buttons, bit);
        return 0;
    case SDL_MOUSEWHEEL: {
        int dy = e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            dy = -dy;
        if (dy && (!look || g_captured))
            wheel_notch(dy > 0 ? 0 : 1, dy < 0 ? -dy : dy);
        return 0;
    }
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            release();
            drop_buttons();
        } else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
            capture();
        }
        return 0;
    case SDL_KEYDOWN:
        if (e->key.keysym.scancode == SDL_SCANCODE_F8 && !e->key.repeat && look) {
            if (g_captured)
                release();
            else
                capture();
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

static void attach_on_event_thread(void *unused)
{
    (void)unused;
    capture();
}

void recomp_mouse_attach(void *hwnd)
{
    if (!hwnd || g_attached || !recomp_mouse_wanted())
        return;
    g_win = (SDL_Window *)hwnd;
    g_attached = 1;
    host_sdl_set_event_hook(sdl_event);
    if (recomp_mouse_look_active())
        fprintf(stderr, "[INPUT] mouse look: %.3f degrees per count (sensitivity %.2f), "
                        "%s%s; click the picture to capture, F8 to release\n",
                RECOMP_MOUSE_DEGREES_PER_COUNT * g_cfg.sensitivity, g_cfg.sensitivity,
                g_cfg.invert_y ? "Y inverted" : "Y not inverted",
                g_cfg.buttons_bound ? "; mouse buttons are bound" : "");
    else
        fprintf(stderr, "[INPUT] mouse buttons are bound; read from the game window\n");
    fflush(stderr);
    /* Take the cursor now if the window came up in front. */
    host_sdl_run_on_event_thread(attach_on_event_thread, NULL);
}

int recomp_mouse_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                intptr_t lp, intptr_t *result)
{
    (void)hwnd; (void)msg; (void)wp; (void)lp; (void)result;
    return 0;
}

#endif
