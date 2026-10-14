/*
 * recomp_mouse.h -- the host mouse: real mouse look, and buttons a binding
 * can name.
 *
 *   - Mouse look. The title's own code takes the mouse movement and turns it
 *     straight into its camera angles (recomp_mouse_take_motion), so aim is
 *     1:1 with the hand: no stick, no stick acceleration, no deadzone. That
 *     needs a title-side hook where the camera is turned -- for Future
 *     Perfect it is a wrapper in titles' recomp_manual.c around the function
 *     that integrates the look input -- and a title that has one says so
 *     with recomp_mouse_title_has_look() before its window opens. A title
 *     without one never captures the cursor.
 *
 *   - Mouse buttons as sources. "mouse:left", "mouse:wheel_up" and the rest
 *     bind to any control exactly as a key does (input_bindings.c).
 *
 * Both are read from raw input on the game's own window, which hands its
 * messages over (recomp_mouse_attach and recomp_mouse_window_message, from
 * src/hle/hle_d3d8.c). Raw input arrives only while that window is in front.
 *
 * While mouse look is active the cursor is clipped to the picture and
 * hidden. F8 lets it go; a click on the picture takes it back (and is not
 * passed to the title).
 */
#ifndef XBOXRECOMP_RECOMP_MOUSE_H
#define XBOXRECOMP_RECOMP_MOUSE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The mouse sources a binding can name, in the order MOUSE_SOURCES in
 * input_bindings.c spells them. Left and right are the primary and
 * secondary buttons as Windows has them, so a left-handed mouse setting
 * carries over. */
enum {
    RECOMP_MOUSE_LEFT, RECOMP_MOUSE_RIGHT, RECOMP_MOUSE_MIDDLE,
    RECOMP_MOUSE_X1, RECOMP_MOUSE_X2,
    RECOMP_MOUSE_WHEEL_UP, RECOMP_MOUSE_WHEEL_DOWN,
    RECOMP_MOUSE_SOURCE_COUNT
};

/* Degrees of turn per mouse count at sensitivity 1.0. The figure most PC
 * shooters use, so a player's usual sensitivity number means what they
 * expect: 0.022 * sensitivity degrees per count. */
#define RECOMP_MOUSE_DEGREES_PER_COUNT 0.022

typedef struct RecompMouseConfig {
    int    look;            /* mouse look wanted (the title must also have it) */
    double sensitivity;     /* RECOMP_MOUSE_DEGREES_PER_COUNT multiplier */
    int    invert_y;        /* mouse forward looks down */
    int    buttons_bound;   /* some control names a mouse: source */
} RecompMouseConfig;

/* Set by recomp_bindings_init() from the config file's "mouse" object and
 * the RECOMP_MOUSE* environment variables. Everything else reads it. */
void recomp_mouse_configure(const RecompMouseConfig *cfg);
const RecompMouseConfig *recomp_mouse_config(void);

/* The title reads mouse motion itself and turns its camera with it. Call
 * before the game window opens (from main), or the first activation will not
 * capture the cursor. */
void recomp_mouse_title_has_look(void);

/* Mouse look is configured on and the title has a hook for it. */
int  recomp_mouse_look_active(void);

/* Mouse look is active, or some control is bound to a mouse button. */
int  recomp_mouse_wanted(void);

/* The cursor is captured: raw movement is being collected for the title. */
int  recomp_mouse_captured(void);

/* The game window, from the thread that owns it, before it is first shown:
 * registers for raw mouse input if the mouse is wanted at all, and says once
 * in the log what the mouse will do. */
void recomp_mouse_attach(void *hwnd);

/* Called first thing in that window's procedure. Returns 1 when the message
 * was answered here (and *result is what to return), 0 to carry on as if
 * this had not been called. WM_INPUT always returns 0: DefWindowProc has to
 * see it to free the input. Off Windows this does nothing. */
int  recomp_mouse_window_message(void *hwnd, unsigned msg, uintptr_t wp,
                                 intptr_t lp, intptr_t *result);

/* Movement in mouse counts, x right and y down, as raw input reports it.
 * The window feeds this while the mouse is captured; scripted input
 * (RECOMP_INPUT_SEQ's mouse_* steps, input replays) feeds it too. */
void recomp_mouse_feed_motion(long dx, long dy);

/* The counts that arrived since the last call, x right and y down (y already
 * flipped when invert_y is set). For the title's look hook, once per game
 * frame. Counts left untaken for more than a quarter of a second -- a pause,
 * a cutscene, a menu, anywhere the hook does not run -- are dropped rather
 * than handed over as one jump when it runs again. Returns 1 if either is
 * non-zero.
 *
 * "Since the last call" ends at the last recomp_mouse_frame_mark when there
 * has been one since then; what came after it is the next frame's. */
int  recomp_mouse_take_motion(long *dx, long *dy);

/* A frame boundary: the title's Swap, after the gate let it through and
 * before the host presents (src/hle/hle_d3d8.c). The game's own clock
 * steps from one Swap to the next, so cutting the mouse here keeps a turn
 * in step with walking and everything else in the frame. Cutting it where
 * the look hook runs instead put the host's lateness into the turn: a
 * present that blocked 20 ms gave the next frame 55 ms of turning while the
 * game moved the player by its usual 33, and turning while walking juddered. */
void recomp_mouse_frame_mark(void);

/* A second copy of every count fed, for RECOMP_INPUT_RECORD (input_host.c):
 * switch it on once, then drain it each pad sample. Taking these does not
 * take what the title's hook will see. */
void recomp_mouse_record_motion(int on);
int  recomp_mouse_take_recorded(long *dx, long *dy);

/* counts * RECOMP_MOUSE_DEGREES_PER_COUNT * sensitivity. */
double recomp_mouse_counts_to_degrees(long counts);

/* One mouse source as a 0..255 magnitude, like any other source. A wheel
 * notch reads as a short press, then a short release, one per notch, so a
 * title that acts on the press edge sees every notch. */
int  recomp_mouse_source(int which);

/* ---- Pointer mode: menus that take the mouse as a pointer ----
 *
 * A title whose menus can follow a pointer turns this on while one is up
 * and off when it goes. On: the cursor is let go (shown, and free to leave
 * the window), mouse buttons and the wheel stop reaching their bindings --
 * a click in a menu is not a shot -- and the title reads the pointer and
 * its clicks below. Off: if the cursor was captured when pointer mode came
 * on it is captured again, the click that is held then not passed on. Any
 * thread; cheap to call every frame with the same value. */
void recomp_mouse_set_pointer_mode(int on);
int  recomp_mouse_pointer_mode(void);

/* Where the picture sits in the window, as fractions of it (x, y, width,
 * height), from the renderer at each present: the pointer is measured
 * against the picture, not the bars around it. */
void recomp_mouse_set_picture(const float rect[4]);

/* The pointer over the picture, 0..1 across and down. Returns 0 (and the
 * nearest point of the picture) when it is outside the picture or the
 * window, or nothing is known yet. */
int  recomp_mouse_pointer(float *x, float *y);

/* Pointer movements since the last call: how a title tells that the mouse,
 * not the pad, is being used. */
long recomp_mouse_pointer_take_moves(void);

/* Pointer-mode buttons pressed since the last call (bit per RECOMP_MOUSE_LEFT
 * ..X2); *held gets those down now, *wheel the wheel notches since the last
 * call (up positive). */
long recomp_mouse_pointer_take_clicks(long *held, int *wheel);

/* Scripted pointer input (RECOMP_INPUT_SEQ's ptr_<x>_<y>, click and rclick
 * steps): the pointer put at a point of the picture (0..1 across and down),
 * and a button pressed or let go (a wheel source turns one notch when
 * pressed), as if the mouse had done it. Outside pointer mode these do
 * nothing. */
void recomp_mouse_script_pointer(float x, float y);
void recomp_mouse_script_button(int which, int down);

#ifdef __cplusplus
}
#endif

#endif
