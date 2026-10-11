/*
 * host_sdl.c - see host_sdl.h.
 *
 * One thread owns SDL: it initialises it, creates the window, and runs the
 * event loop for the life of the process, which is what the Win32 window
 * thread in src/hle does on Windows. Everything other threads ask -- is a key
 * down, is the window in front, what is pad 2 doing -- is answered from state
 * that loop keeps, so no guest thread ever calls into SDL's video layer.
 */
#if !defined(_WIN32)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "host_sdl.h"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PADS 4

static pthread_once_t   s_once = PTHREAD_ONCE_INIT;
static pthread_t        s_thread;
static pthread_mutex_t  s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   s_cond = PTHREAD_COND_INITIALIZER;
static int              s_ready;            /* event thread has initialised SDL */
static int              s_video;            /* SDL video came up */
static Uint32           s_call_event;       /* SDL_RegisterEvents: run a function */
static SDL_Window      *s_window;
static volatile int     s_focused;
static volatile int     s_draw_w, s_draw_h;     /* drawable pixels, from the event thread */
static host_sdl_event_hook s_hook;
static void           (*s_close_hook)(void);

/* Keys and mouse buttons by Win32 virtual-key code, as a count of physical
 * keys holding each down (Return and keypad Enter are both VK_RETURN). */
static volatile unsigned char s_vk[256];

/* Gamepads in the order they were connected, XInput style. */
static SDL_GameController *s_pads[PADS];
static pthread_mutex_t     s_pad_lock = PTHREAD_MUTEX_INITIALIZER;

static int on_event_thread(void)
{
    return s_ready && pthread_equal(pthread_self(), s_thread);
}

/* ---- keys ---------------------------------------------------------------- */

static int vk_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0:            return '0';
    case SDL_SCANCODE_RETURN:       return 0x0D;
    case SDL_SCANCODE_KP_ENTER:     return 0x0D;
    case SDL_SCANCODE_ESCAPE:       return 0x1B;
    case SDL_SCANCODE_BACKSPACE:    return 0x08;
    case SDL_SCANCODE_TAB:          return 0x09;
    case SDL_SCANCODE_SPACE:        return 0x20;
    case SDL_SCANCODE_MINUS:        return 0xBD;
    case SDL_SCANCODE_EQUALS:       return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET:  return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH:    return 0xDC;
    case SDL_SCANCODE_SEMICOLON:    return 0xBA;
    case SDL_SCANCODE_APOSTROPHE:   return 0xDE;
    case SDL_SCANCODE_GRAVE:        return 0xC0;
    case SDL_SCANCODE_COMMA:        return 0xBC;
    case SDL_SCANCODE_PERIOD:       return 0xBE;
    case SDL_SCANCODE_SLASH:        return 0xBF;
    case SDL_SCANCODE_CAPSLOCK:     return 0x14;
    case SDL_SCANCODE_PRINTSCREEN:  return 0x2C;
    case SDL_SCANCODE_SCROLLLOCK:   return 0x91;
    case SDL_SCANCODE_PAUSE:        return 0x13;
    case SDL_SCANCODE_INSERT:       return 0x2D;
    case SDL_SCANCODE_HOME:         return 0x24;
    case SDL_SCANCODE_PAGEUP:       return 0x21;
    case SDL_SCANCODE_DELETE:       return 0x2E;
    case SDL_SCANCODE_END:          return 0x23;
    case SDL_SCANCODE_PAGEDOWN:     return 0x22;
    case SDL_SCANCODE_RIGHT:        return 0x27;
    case SDL_SCANCODE_LEFT:         return 0x25;
    case SDL_SCANCODE_DOWN:         return 0x28;
    case SDL_SCANCODE_UP:           return 0x26;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 0x90;
    case SDL_SCANCODE_KP_DIVIDE:    return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY:  return 0x6A;
    case SDL_SCANCODE_KP_MINUS:     return 0x6D;
    case SDL_SCANCODE_KP_PLUS:      return 0x6B;
    case SDL_SCANCODE_KP_0:         return 0x60;
    case SDL_SCANCODE_KP_PERIOD:    return 0x6E;
    case SDL_SCANCODE_LCTRL:        return 0xA2;
    case SDL_SCANCODE_RCTRL:        return 0xA3;
    case SDL_SCANCODE_LSHIFT:       return 0xA0;
    case SDL_SCANCODE_RSHIFT:       return 0xA1;
    case SDL_SCANCODE_LALT:         return 0xA4;
    case SDL_SCANCODE_RALT:         return 0xA5;
    default:                        return 0;
    }
}

/* The side-less codes Windows also reports: VK_SHIFT for either shift. */
static int vk_generic(int vk)
{
    switch (vk) {
    case 0xA0: case 0xA1: return 0x10;
    case 0xA2: case 0xA3: return 0x11;
    case 0xA4: case 0xA5: return 0x12;
    default:              return 0;
    }
}

static void vk_change(int vk, int down)
{
    if (vk <= 0 || vk > 255)
        return;
    if (down)
        s_vk[vk]++;
    else if (s_vk[vk])
        s_vk[vk]--;
}

static int vk_from_mouse_button(Uint8 b)
{
    switch (b) {
    case SDL_BUTTON_LEFT:   return 0x01;
    case SDL_BUTTON_RIGHT:  return 0x02;
    case SDL_BUTTON_MIDDLE: return 0x04;
    case SDL_BUTTON_X1:     return 0x05;
    case SDL_BUTTON_X2:     return 0x06;
    default:                return 0;
    }
}

SHORT GetAsyncKeyState(int vKey)
{
    if (vKey <= 0 || vKey > 255 || !s_focused)
        return 0;
    return s_vk[vKey] ? (SHORT)0x8000 : 0;
}

HWND GetForegroundWindow(void)
{
    return s_focused ? (HWND)s_window : NULL;
}

DWORD GetWindowThreadProcessId(HWND hwnd, LPDWORD process_id)
{
    int ours = hwnd && hwnd == (HWND)s_window;

    if (process_id)
        *process_id = ours ? (DWORD)getpid() : 0;
    return ours ? 1 : 0;
}

BOOL GetClientRect(HWND hwnd, LPRECT rc)
{
    if (!rc || !hwnd || hwnd != (HWND)s_window)
        return FALSE;
    rc->left = rc->top = 0;
    rc->right = s_draw_w;
    rc->bottom = s_draw_h;
    return TRUE;
}

BOOL IsIconic(HWND hwnd)
{
    return hwnd && hwnd == (HWND)s_window &&
           (SDL_GetWindowFlags(s_window) & SDL_WINDOW_MINIMIZED) != 0;
}

/* ---- gamepads ------------------------------------------------------------ */

static void pad_added(int device_index)
{
    SDL_GameController *gc;
    SDL_JoystickID id;
    int i;

    if (!SDL_IsGameController(device_index))
        return;
    gc = SDL_GameControllerOpen(device_index);
    if (!gc)
        return;
    id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
    pthread_mutex_lock(&s_pad_lock);
    for (i = 0; i < PADS; i++)
        if (s_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == id)
            break;
    if (i == PADS) {
        for (i = 0; i < PADS && s_pads[i]; i++)
            ;
        if (i < PADS) {
            s_pads[i] = gc;
            gc = NULL;
            fprintf(stderr, "[INPUT] gamepad %d: %s\n", i,
                    SDL_GameControllerName(s_pads[i]) ? SDL_GameControllerName(s_pads[i]) : "?");
            fflush(stderr);
        }
    }
    pthread_mutex_unlock(&s_pad_lock);
    if (gc)
        SDL_GameControllerClose(gc);
}

static void pad_removed(SDL_JoystickID id)
{
    int i;

    pthread_mutex_lock(&s_pad_lock);
    for (i = 0; i < PADS; i++) {
        if (s_pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pads[i])) == id) {
            SDL_GameControllerClose(s_pads[i]);
            s_pads[i] = NULL;
            fprintf(stderr, "[INPUT] gamepad %d disconnected\n", i);
            fflush(stderr);
        }
    }
    pthread_mutex_unlock(&s_pad_lock);
}

static SHORT stick(SDL_GameController *gc, SDL_GameControllerAxis a, int flip)
{
    int v = SDL_GameControllerGetAxis(gc, a);

    if (flip)                           /* SDL's y grows down, XInput's up */
        v = -v - 1;
    if (v > 32767)  v = 32767;
    if (v < -32768) v = -32768;
    return (SHORT)v;
}

static BYTE trigger(SDL_GameController *gc, SDL_GameControllerAxis a)
{
    int v = SDL_GameControllerGetAxis(gc, a);

    return (BYTE)(v <= 0 ? 0 : v * 255 / 32767);
}

DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{
    static const struct { SDL_GameControllerButton b; WORD bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, XINPUT_GAMEPAD_A },
        { SDL_CONTROLLER_BUTTON_B, XINPUT_GAMEPAD_B },
        { SDL_CONTROLLER_BUTTON_X, XINPUT_GAMEPAD_X },
        { SDL_CONTROLLER_BUTTON_Y, XINPUT_GAMEPAD_Y },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER },
        { SDL_CONTROLLER_BUTTON_START, XINPUT_GAMEPAD_START },
        { SDL_CONTROLLER_BUTTON_BACK, XINPUT_GAMEPAD_BACK },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, XINPUT_GAMEPAD_LEFT_THUMB },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, XINPUT_GAMEPAD_RIGHT_THUMB },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, XINPUT_GAMEPAD_DPAD_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
    };
    static DWORD packet;
    SDL_GameController *gc;
    size_t i;

    if (state)
        memset(state, 0, sizeof *state);
    host_sdl_start();
    if (idx >= PADS || !state)
        return ERROR_DEVICE_NOT_CONNECTED;
    pthread_mutex_lock(&s_pad_lock);
    gc = s_pads[idx];
    if (gc) {
        XINPUT_GAMEPAD *g = &state->Gamepad;
        for (i = 0; i < sizeof map / sizeof map[0]; i++)
            if (SDL_GameControllerGetButton(gc, map[i].b))
                g->wButtons |= map[i].bit;
        g->bLeftTrigger  = trigger(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        g->bRightTrigger = trigger(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        g->sThumbLX = stick(gc, SDL_CONTROLLER_AXIS_LEFTX, 0);
        g->sThumbLY = stick(gc, SDL_CONTROLLER_AXIS_LEFTY, 1);
        g->sThumbRX = stick(gc, SDL_CONTROLLER_AXIS_RIGHTX, 0);
        g->sThumbRY = stick(gc, SDL_CONTROLLER_AXIS_RIGHTY, 1);
        state->dwPacketNumber = ++packet;
    }
    pthread_mutex_unlock(&s_pad_lock);
    return gc ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

/* ---- the event thread ---------------------------------------------------- */

typedef struct {
    void (*fn)(void *);
    void *arg;
    int done;
} host_call;

static void handle_event(const SDL_Event *e)
{
    int vk;

    if (s_hook && s_hook(e))
        return;
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (e->key.repeat)
            break;
        vk = vk_from_scancode(e->key.keysym.scancode);
        vk_change(vk, e->type == SDL_KEYDOWN);
        vk_change(vk_generic(vk), e->type == SDL_KEYDOWN);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        vk_change(vk_from_mouse_button(e->button.button),
                  e->type == SDL_MOUSEBUTTONDOWN);
        break;
    case SDL_WINDOWEVENT:
        switch (e->window.event) {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            s_focused = 1;
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            /* The key-ups now go to someone else. */
            s_focused = 0;
            memset((void *)s_vk, 0, sizeof s_vk);
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SHOWN: {
            int w = 0, h = 0;
            if (s_window)
                SDL_Vulkan_GetDrawableSize(s_window, &w, &h);
            s_draw_w = w;
            s_draw_h = h;
            break;
        }
        case SDL_WINDOWEVENT_CLOSE:
            if (s_close_hook)
                s_close_hook();
            else
                exit(0);
            break;
        default:
            break;
        }
        break;
    case SDL_QUIT:
        if (s_close_hook)
            s_close_hook();
        else
            exit(0);
        break;
    case SDL_CONTROLLERDEVICEADDED:
        pad_added(e->cdevice.which);
        break;
    case SDL_CONTROLLERDEVICEREMOVED:
        pad_removed(e->cdevice.which);
        break;
    default:
        if (s_call_event && e->type == s_call_event) {
            host_call *c = (host_call *)e->user.data1;
            c->fn(c->arg);
            pthread_mutex_lock(&s_lock);
            c->done = 1;
            pthread_cond_broadcast(&s_cond);
            pthread_mutex_unlock(&s_lock);
        }
        break;
    }
}

void host_sdl_hints(void)
{
    /* The runtime handles its own signals (win32_compat.c). */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    /* A pad keeps working while another window has the focus; the bindings
     * decide whether to listen (window_has_focus in input_bindings.c). */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
}

static void *event_thread(void *unused)
{
    SDL_Event e;

    (void)unused;
    host_sdl_hints();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0) {
        s_video = 1;
    } else {
        fprintf(stderr, "[HOST] SDL video unavailable (%s); no window, pads only\n",
                SDL_GetError());
        if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
            fprintf(stderr, "[HOST] SDL_Init failed: %s\n", SDL_GetError());
    }
    fflush(stderr);
    s_call_event = SDL_RegisterEvents(1);
    if (s_call_event == (Uint32)-1)
        s_call_event = 0;

    pthread_mutex_lock(&s_lock);
    s_ready = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);

    for (;;) {
        if (SDL_WaitEventTimeout(&e, 20)) {
            do
                handle_event(&e);
            while (SDL_PollEvent(&e));
        }
    }
    return NULL;
}

static void start_once(void)
{
    pthread_create(&s_thread, NULL, event_thread, NULL);
    pthread_mutex_lock(&s_lock);
    while (!s_ready)
        pthread_cond_wait(&s_cond, &s_lock);
    pthread_mutex_unlock(&s_lock);
}

void host_sdl_start(void)
{
    pthread_once(&s_once, start_once);
}

void host_sdl_run_on_event_thread(void (*fn)(void *), void *arg)
{
    host_call c;
    SDL_Event e;

    host_sdl_start();
    if (on_event_thread() || !s_call_event) {
        fn(arg);
        return;
    }
    c.fn = fn;
    c.arg = arg;
    c.done = 0;
    memset(&e, 0, sizeof e);
    e.type = s_call_event;
    e.user.data1 = &c;
    if (SDL_PushEvent(&e) != 1) {
        fn(arg);
        return;
    }
    pthread_mutex_lock(&s_lock);
    while (!c.done)
        pthread_cond_wait(&s_cond, &s_lock);
    pthread_mutex_unlock(&s_lock);
}

typedef struct {
    unsigned width, height;
    const char *title;
} window_request;

static void create_window(void *p)
{
    window_request *r = (window_request *)p;
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;

    if (s_window || !s_video)
        return;
    s_window = SDL_CreateWindow(r->title ? r->title : "xboxrecomp",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                (int)r->width, (int)r->height, flags);
    if (!s_window) {
        fprintf(stderr, "[HOST] SDL_CreateWindow failed: %s\n", SDL_GetError());
        fflush(stderr);
        return;
    }
    SDL_RaiseWindow(s_window);
    {
        int w = 0, h = 0;
        SDL_Vulkan_GetDrawableSize(s_window, &w, &h);
        s_draw_w = w;
        s_draw_h = h;
    }
    s_focused = (SDL_GetWindowFlags(s_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

HWND host_sdl_window_create(unsigned width, unsigned height, const char *title)
{
    window_request r;

    r.width = width ? width : 640;
    r.height = height ? height : 480;
    r.title = title;
    host_sdl_run_on_event_thread(create_window, &r);
    return (HWND)s_window;
}

HWND host_sdl_window(void)
{
    return (HWND)s_window;
}

static void set_title(void *p)
{
    if (s_window)
        SDL_SetWindowTitle(s_window, (const char *)p);
}

void host_sdl_set_title(const char *title)
{
    if (title && s_window)
        host_sdl_run_on_event_thread(set_title, (void *)title);
}

void host_sdl_set_event_hook(host_sdl_event_hook hook)
{
    s_hook = hook;
}

void host_sdl_set_close_hook(void (*hook)(void))
{
    s_close_hook = hook;
}

#endif /* !_WIN32 */
