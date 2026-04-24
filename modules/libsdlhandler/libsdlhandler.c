/*
 *  Copyright © 2006-2012 SplinterGU (Fenix/Bennugd)
 *
 *  This file is part of Bennu - Game Development
 *
 *  This software is provided 'as-is', without any express or implied
 *  warranty. In no event will the authors be held liable for any damages
 *  arising from the use of this software.
 *
 *  Permission is granted to anyone to use this software for any purpose,
 *  including commercial applications, and to alter it and redistribute it
 *  freely, subject to the following restrictions:
 *
 *     1. The origin of this software must not be misrepresented; you must not
 *     claim that you wrote the original software. If you use this software
 *     in a product, an acknowledgment in the product documentation would be
 *     appreciated but is not required.
 *
 *     2. Altered source versions must be plainly marked as such, and must not be
 *     misrepresented as being the original software.
 *
 *     3. This notice may not be removed or altered from any source
 *     distribution.
 *
 */

#include "bgddl.h"

#include <SDL3/SDL.h>

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#endif

/* ----------------------------------------------------------------- */
/* Direct /dev/input/event* keyboard polling — bypasses SDL3's own
 * evdev integration, which on the Amlogic-old Mali FBDEV + gptokeyb
 * combo sometimes drops events (multiple readers racing on the same
 * fd, or hotplug missing gptokeyb's uinput-created virtual kbd).
 *
 * We scan /dev/input/event* at init, open every device that has EV_KEY
 * with at least one "keyboard-ish" key in the lower block. Then each
 * frame we read() events non-blocking and forward EV_KEY (press/release)
 * to SDL3 via SDL_SendKeyboardKey using the evdev scancode directly. */

#ifdef __linux__

#define MAX_EV_DEVS 16
#define BITS_PER_LONG (8 * sizeof(long))
#define NBITS(x)     (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define test_bit(bit, array) ((array[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1)

static int ev_fds[MAX_EV_DEVS];
static int ev_fd_count = 0;

/* Linux evdev keycode -> SDL3 scancode (0..255 range). Populated from the
 * SDL3 SDL_EVDEV_translate_keycode equivalents. Same mapping as xkb
 * evdev layout; mostly one-to-one for the standard keyboard region. */
static SDL_Scancode evdev_to_sdl_scancode(int kc)
{
    /* Small handwritten table for the keys gptokeyb maps by default. */
    switch (kc) {
        case 1:   return SDL_SCANCODE_ESCAPE;
        case 2:   return SDL_SCANCODE_1;
        case 3:   return SDL_SCANCODE_2;
        case 4:   return SDL_SCANCODE_3;
        case 5:   return SDL_SCANCODE_4;
        case 6:   return SDL_SCANCODE_5;
        case 7:   return SDL_SCANCODE_6;
        case 8:   return SDL_SCANCODE_7;
        case 9:   return SDL_SCANCODE_8;
        case 10:  return SDL_SCANCODE_9;
        case 11:  return SDL_SCANCODE_0;
        case 12:  return SDL_SCANCODE_MINUS;
        case 13:  return SDL_SCANCODE_EQUALS;
        case 14:  return SDL_SCANCODE_BACKSPACE;
        case 15:  return SDL_SCANCODE_TAB;
        case 16:  return SDL_SCANCODE_Q;
        case 17:  return SDL_SCANCODE_W;
        case 18:  return SDL_SCANCODE_E;
        case 19:  return SDL_SCANCODE_R;
        case 20:  return SDL_SCANCODE_T;
        case 21:  return SDL_SCANCODE_Y;
        case 22:  return SDL_SCANCODE_U;
        case 23:  return SDL_SCANCODE_I;
        case 24:  return SDL_SCANCODE_O;
        case 25:  return SDL_SCANCODE_P;
        case 26:  return SDL_SCANCODE_LEFTBRACKET;
        case 27:  return SDL_SCANCODE_RIGHTBRACKET;
        case 28:  return SDL_SCANCODE_RETURN;
        case 29:  return SDL_SCANCODE_LCTRL;
        case 30:  return SDL_SCANCODE_A;
        case 31:  return SDL_SCANCODE_S;
        case 32:  return SDL_SCANCODE_D;
        case 33:  return SDL_SCANCODE_F;
        case 34:  return SDL_SCANCODE_G;
        case 35:  return SDL_SCANCODE_H;
        case 36:  return SDL_SCANCODE_J;
        case 37:  return SDL_SCANCODE_K;
        case 38:  return SDL_SCANCODE_L;
        case 39:  return SDL_SCANCODE_SEMICOLON;
        case 40:  return SDL_SCANCODE_APOSTROPHE;
        case 41:  return SDL_SCANCODE_GRAVE;
        case 42:  return SDL_SCANCODE_LSHIFT;
        case 43:  return SDL_SCANCODE_BACKSLASH;
        case 44:  return SDL_SCANCODE_Z;
        case 45:  return SDL_SCANCODE_X;
        case 46:  return SDL_SCANCODE_C;
        case 47:  return SDL_SCANCODE_V;
        case 48:  return SDL_SCANCODE_B;
        case 49:  return SDL_SCANCODE_N;
        case 50:  return SDL_SCANCODE_M;
        case 51:  return SDL_SCANCODE_COMMA;
        case 52:  return SDL_SCANCODE_PERIOD;
        case 53:  return SDL_SCANCODE_SLASH;
        case 54:  return SDL_SCANCODE_RSHIFT;
        case 55:  return SDL_SCANCODE_KP_MULTIPLY;
        case 56:  return SDL_SCANCODE_LALT;
        case 57:  return SDL_SCANCODE_SPACE;
        case 58:  return SDL_SCANCODE_CAPSLOCK;
        case 59:  return SDL_SCANCODE_F1;
        case 60:  return SDL_SCANCODE_F2;
        case 61:  return SDL_SCANCODE_F3;
        case 62:  return SDL_SCANCODE_F4;
        case 63:  return SDL_SCANCODE_F5;
        case 64:  return SDL_SCANCODE_F6;
        case 65:  return SDL_SCANCODE_F7;
        case 66:  return SDL_SCANCODE_F8;
        case 67:  return SDL_SCANCODE_F9;
        case 68:  return SDL_SCANCODE_F10;
        case 97:  return SDL_SCANCODE_RCTRL;
        case 100: return SDL_SCANCODE_RALT;
        case 103: return SDL_SCANCODE_UP;
        case 105: return SDL_SCANCODE_LEFT;
        case 106: return SDL_SCANCODE_RIGHT;
        case 108: return SDL_SCANCODE_DOWN;
        default:  return SDL_SCANCODE_UNKNOWN;
    }
}

static int device_is_keyboard(int fd)
{
    unsigned long evbit[NBITS(EV_MAX)] = {0};
    unsigned long keybit[NBITS(KEY_MAX)] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) return 0;
    if (!test_bit(EV_KEY, evbit)) return 0;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) return 0;

    /* Needs at least ENTER (28) or some letter — rules out pure joysticks */
    if (test_bit(KEY_ENTER, keybit) || test_bit(KEY_A, keybit) ||
        test_bit(KEY_SPACE, keybit) || test_bit(KEY_ESC, keybit))
        return 1;
    return 0;
}

static void scan_evdev_keyboards(void)
{
    ev_fd_count = 0;
    for (int i = 0; i < 32 && ev_fd_count < MAX_EV_DEVS; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!device_is_keyboard(fd)) { close(fd); continue; }
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        /* EVIOCGRAB(1): take exclusive read access. Without this, SDL3's
         * own EVDEV backend also reads from /dev/input/eventN and pushes
         * its own KEY_DOWN/KEY_UP events into the queue — together with
         * the ones we SDL_PushEvent below, that's a double-delivery bug
         * ("1 tecla duplicada"). Grabbing serializes the device to us. */
        if (ioctl(fd, EVIOCGRAB, 1) < 0) {
            SDL_Log("libsdlhandler: EVIOCGRAB on %s failed (errno=%d) — SDL3 may double-deliver", path, errno);
        } else {
            SDL_Log("libsdlhandler: grabbed %s (%s) exclusively", path, name);
        }

        ev_fds[ev_fd_count++] = fd;
    }
    SDL_Log("libsdlhandler: total direct evdev kbds = %d", ev_fd_count);
}

static void poll_evdev_keys(void)
{
    struct input_event events[32];
    for (int i = 0; i < ev_fd_count; i++) {
        ssize_t len;
        while ((len = read(ev_fds[i], events, sizeof(events))) > 0) {
            int n = len / sizeof(events[0]);
            for (int j = 0; j < n; j++) {
                struct input_event *e = &events[j];
                if (e->type != EV_KEY) continue;
                if (e->value != 0 && e->value != 1) continue; /* skip autorepeat(2) */
                SDL_Scancode sc = evdev_to_sdl_scancode(e->code);
                if (sc == SDL_SCANCODE_UNKNOWN) continue;
                /* Push a synthesized keyboard event into SDL3's queue.
                 * libkey's process_key_events uses SDL_PeepEvents to pull
                 * these back out — so forwarding via SDL_PushEvent lets the
                 * rest of the keyboard pipeline work unchanged. */
                SDL_Event ev;
                SDL_zero(ev);
                ev.type = e->value ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                ev.key.timestamp = SDL_GetTicksNS();
                ev.key.which = (SDL_KeyboardID)(uintptr_t)(ev_fds[i] + 1);
                ev.key.scancode = sc;
                ev.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
                ev.key.mod = SDL_GetModState();
                ev.key.raw = e->code;
                ev.key.down = e->value ? true : false;
                ev.key.repeat = false;
                SDL_PushEvent(&ev);
            }
        }
    }
}

static int g_direct_evdev_initialized = 0;
static int g_rescan_counter = 0;

static void rescan_if_needed(void)
{
    /* Every ~60 polls (~1 sec at 60 FPS) close+rescan so newly
     * hotplugged evdev devices (e.g. gptokeyb virtual kbd created
     * after bgdi init) are picked up. Cheap: ioctl+close. */
    if (++g_rescan_counter < 60) return;
    g_rescan_counter = 0;
    /* Count current /dev/input/event* that look like keyboards */
    int found = 0;
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        int is_kbd = device_is_keyboard(fd);
        close(fd);
        if (is_kbd) found++;
    }
    if (found == ev_fd_count) return;  /* no change */
    /* Close & re-scan */
    for (int i = 0; i < ev_fd_count; i++) close(ev_fds[i]);
    ev_fd_count = 0;
    scan_evdev_keyboards();
}
#endif /* __linux__ */

/* ----------------------------------------------------------------- */

static void  dump_new_events()
{
#ifdef __linux__
    if (!g_direct_evdev_initialized) {
        g_direct_evdev_initialized = 1;
        /* Enable direct evdev reader by default on Mali FBDEV (Amlogic),
         * where SDL3's own evdev integration drops events randomly when a
         * gptokeyb uinput virtual keyboard is involved. Opt out with
         * BGD_DIRECT_EVDEV=0. */
        const char *opt = SDL_getenv("BGD_DIRECT_EVDEV");
        int enable = 1;
        if (opt && (opt[0] == '0' || opt[0] == 'n' || opt[0] == 'N'))
            enable = 0;
        else {
            const char *vd = SDL_GetCurrentVideoDriver();
            /* If no Mali driver and no explicit opt-in, stay out of the way. */
            if ((!vd || SDL_strcmp(vd, "mali") != 0) && !SDL_getenv("BGD_DISABLE_SDL_JOYSTICK"))
                enable = 0;
        }
        if (enable) scan_evdev_keyboards();
    }
    if (ev_fd_count > 0) { rescan_if_needed(); poll_evdev_keys(); }
#endif
    SDL_PumpEvents();
}

/* ----------------------------------------------------------------- */

void __bgdexport( libsdlhandler, module_initialize )()
{
}

void __bgdexport( libsdlhandler, module_finalize )()
{
#ifdef __linux__
    for (int i = 0; i < ev_fd_count; i++) close(ev_fds[i]);
    ev_fd_count = 0;
#endif
}

/* ----------------------------------------------------------------- */

HOOK __bgdexport( libsdlhandler, handler_hooks )[] =
{
    { 5000, dump_new_events                   },
    {    0, NULL                              }
} ;
