/*
 * recomp_setup.h -- the window a shared build shows before the game, the
 * first time it runs: choose your disc, then wait while the game module is
 * built from it (src/kernel/recomp_module.h, scripts/build_module.py).
 *
 * One small SDL window with the desktop's own fonts; it is closed again
 * before the runtime opens the game window. Every call works without a
 * display too, falling back to the terminal, so a headless or broken setup
 * still says what is wrong.
 *
 * Linux only for now: on Windows the game is still linked into the
 * executable (XBOXRECOMP_GAME_MODULE is off), so nothing calls this.
 */
#ifndef XBOXRECOMP_RECOMP_SETUP_H
#define XBOXRECOMP_RECOMP_SETUP_H

#include <stddef.h>

/* Open the window. Returns 0 when there is no display (the calls below
 * then use stdin/stderr). */
int  recomp_setup_open(const char *window_title);
void recomp_setup_close(void);

/* Checks a path the player chose. Returns 1 if it will do; otherwise 0 with
 * a sentence in msg. May be slow (it runs while "Checking..." shows). */
typedef int (*recomp_setup_validate_fn)(const char *path, char *msg, size_t msg_bytes,
                                        void *ctx);

/* Ask for the game: a disc image or a folder, typed, pasted, dropped on the
 * window or picked with the desktop's file dialog. Returns 1 with the
 * accepted path in out, 0 if the player quit. `error` (may be NULL) is shown
 * from the start, e.g. why the last choice no longer works. */
int  recomp_setup_pick(const char *heading, const char *body, const char *error,
                       recomp_setup_validate_fn validate, void *ctx,
                       char *out, size_t out_bytes);

/* Run argv[0] (searched on PATH) with argv, showing its progress: lines
 * "@progress <from> <to> <what>" move the bar, a line starting "error:" is
 * what a failure says, and everything goes to log_path. On failure the
 * player can retry (it runs again) or quit. Returns 1 when the program
 * exited 0, 0 if the player gave up. */
int  recomp_setup_run(const char *heading, const char *body, char *const argv[],
                      const char *log_path);

/* Run argv to completion without showing anything; its last line starting
 * "error:" (else its last line) in msg. Returns the exit status, or -1 if it
 * could not be started (msg says why). */
int  recomp_setup_capture(char *const argv[], char *msg, size_t msg_bytes);

/* A message with an OK button. */
void recomp_setup_message(const char *heading, const char *body);

#endif /* XBOXRECOMP_RECOMP_SETUP_H */
