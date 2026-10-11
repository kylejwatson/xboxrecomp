/*
 * host_sdl.h - the game window, keyboard and gamepads on a POSIX host.
 *
 * On Windows the HLE opens a Win32 window on a thread of its own and pumps it
 * there, the input bindings read GetAsyncKeyState and XInputGetState, and the
 * mouse reads raw input. Here one SDL thread does all of that: it initialises
 * SDL, owns the window and runs the event loop, and the Win32 calls the shared
 * code makes (GetAsyncKeyState, GetForegroundWindow, XInputGetState) are
 * answered from what that loop saw. The rest of the runtime is unchanged.
 *
 * The window handle handed around as an HWND is the SDL_Window*.
 */
#ifndef XBOXRECOMP_HOST_SDL_H
#define XBOXRECOMP_HOST_SDL_H

#if !defined(_WIN32)

#include "xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens the game window (client area width x height, resizable, Vulkan
 * capable) and returns it, starting the event thread if it is not running.
 * NULL if SDL could not make one -- no display, no Vulkan loader. One window
 * per process; a second call returns the first. */
HWND host_sdl_window_create(unsigned width, unsigned height, const char *title);

/* The window, or NULL before host_sdl_window_create. */
HWND host_sdl_window(void);

/* The window's title; safe from any thread. */
void host_sdl_set_title(const char *title);

/* Hints every SDL user in the process needs before its first SDL_Init*: the
 * runtime owns the process's signals (SIGTERM must still kill it; SDL would
 * otherwise turn it into SDL_QUIT). Idempotent. */
void host_sdl_hints(void);

/* Starts the event thread without a window, so gamepads are read even when
 * nothing is drawn (RECOMP_HLE_D3D8=off). Called by XInputGetState. */
void host_sdl_start(void);

/* Called on the event thread with each SDL_Event (as a const void *) before
 * the module looks at it; the mouse code takes its motion and buttons here.
 * One hook. Return non-zero to swallow the event. */
typedef int (*host_sdl_event_hook)(const void *sdl_event);
void host_sdl_set_event_hook(host_sdl_event_hook hook);

/* Called on the event thread when the user closes the window. Without one
 * the process exits. */
void host_sdl_set_close_hook(void (*hook)(void));

/* Runs fn(arg) on the event thread and waits for it: SDL video calls
 * (relative mouse mode, cursor) belong to the thread that initialised
 * video. Runs fn directly when called from that thread. */
void host_sdl_run_on_event_thread(void (*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* XBOXRECOMP_HOST_SDL_H */
