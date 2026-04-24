/*
 *  Copyright � 2006-2012 SplinterGU (Fenix/Bennugd)
 *  Copyright � 2002-2006 Fenix Team (Fenix)
 *  Copyright � 1999-2002 Jos� Luis Cebri�n Pag�e (Fenix)
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

/* --------------------------------------------------------------------------- */
/* Thanks Sandman for suggest on openjoys at initialization time               */
/* --------------------------------------------------------------------------- */
/* Credits SplinterGU/Sandman 2007-2009                                        */
/* --------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdbool.h>
#include <SDL3/SDL.h>

/* --------------------------------------------------------------------------- */
/* Direct evdev reader — workaround for SDL3 lib32 (armhf) which opens the
 * joystick fd fine but never drains events, so SDL_GetJoystickButton stays
 * stuck at the initial poll (axis midpoints, all buttons released). We keep
 * our own state arrays for every gamepad-class /dev/input/event*, read
 * raw input_events each frame in __libjoy_handle_tick(), and have all the
 * JOY_GET* functions prefer our cache over SDL's.                             */
#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>
#define LIBJOY_DIRECT_EVDEV 1
#define LIBJOY_MAX_BTN  32
#define LIBJOY_MAX_AXIS 16
#define LIBJOY_MAX_HAT  4
typedef struct {
    int fd;                                    /* -1 = slot unused */
    int buttons[LIBJOY_MAX_BTN];               /* 0/1 */
    int axes[LIBJOY_MAX_AXIS];                 /* scaled to SDL range (-32768..32767) */
    int hats[LIBJOY_MAX_HAT];                  /* SDL_HAT_* mask */
    int btn_map[KEY_MAX];                      /* KEY_* / BTN_* → index into buttons[] */
    int abs_map[ABS_MAX];                      /* ABS_* → index into axes[] OR hats */
    int abs_hat[ABS_MAX];                      /* 1 if ABS_HAT0X/Y, stored as hat idx */
    int abs_min[ABS_MAX], abs_max[ABS_MAX];
    int nbtn, naxis, nhat;
} libjoy_rawpad_t;
static libjoy_rawpad_t _rawpads[32];
#endif

/* --------------------------------------------------------------------------- */

#include "bgddl.h"

#include "bgdrtm.h"

#include "files.h"
#include "xstrings.h"

/* --------------------------------------------------------------------------- */

#ifdef TARGET_CAANOO
#include "caanoo/te9_tf9_hybrid_driver.c"

#ifndef ABS
#define ABS(x) (((x) < 0) ? -(x):(x))
#endif

#endif

/* --------------------------------------------------------------------------- */

#define MAX_JOYS    32

static int _max_joys = 0;
static SDL_Joystick * _joysticks[MAX_JOYS];
static int _selected_joystick = -1;

#ifdef LIBJOY_DIRECT_EVDEV
#ifndef NBITS
#define NBITS(x) (((x) + (8 * sizeof(long)) - 1) / (8 * sizeof(long)))
#endif
#ifndef test_bit
#define test_bit(bit, array) ((((const unsigned long*)array)[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1)
#endif

static void libjoy_rawpad_reset(libjoy_rawpad_t *p) {
    p->fd = -1;
    p->nbtn = p->naxis = p->nhat = 0;
    for (int i = 0; i < LIBJOY_MAX_BTN;  i++) p->buttons[i] = 0;
    for (int i = 0; i < LIBJOY_MAX_AXIS; i++) p->axes[i] = 0;
    for (int i = 0; i < LIBJOY_MAX_HAT;  i++) p->hats[i] = SDL_HAT_CENTERED;
    for (int i = 0; i < KEY_MAX; i++) p->btn_map[i] = -1;
    for (int i = 0; i < ABS_MAX; i++) { p->abs_map[i] = -1; p->abs_hat[i] = -1; }
}

/* Scan /dev/input/event* and open every device that looks like a gamepad
 * (has EV_KEY with at least one BTN_JOYSTICK..BTN_GEAR_UP or BTN_GAMEPAD).
 * Returns number of opened pads. */
static int libjoy_scan_pads(void) {
    int count = 0;
    for (int n = 0; n < 32 && count < MAX_JOYS; n++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", n);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        unsigned long evbit[NBITS(EV_MAX)]  = {0};
        unsigned long keybit[NBITS(KEY_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0
         || !test_bit(EV_KEY, evbit)) { close(fd); continue; }
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) {
            close(fd); continue;
        }

        int is_pad = 0;
        for (int k = BTN_JOYSTICK; k <= BTN_GEAR_UP; k++) if (test_bit(k, keybit)) { is_pad = 1; break; }
        if (!is_pad) { close(fd); continue; }

        libjoy_rawpad_t *p = &_rawpads[count];
        libjoy_rawpad_reset(p);
        p->fd = fd;

        /* Map each real KEY bit to a sequential button index in OUR array.
         * This is the same mapping SDL3 uses (BTN_JOYSTICK=1st, then
         * BTN_GAMEPAD range, then extras). */
        int bidx = 0;
        for (int k = BTN_JOYSTICK; k < BTN_GAMEPAD && bidx < LIBJOY_MAX_BTN; k++) {
            if (test_bit(k, keybit)) { p->btn_map[k] = bidx++; }
        }
        for (int k = BTN_GAMEPAD; k <= BTN_THUMBR && bidx < LIBJOY_MAX_BTN; k++) {
            if (test_bit(k, keybit)) { p->btn_map[k] = bidx++; }
        }
        for (int k = BTN_DPAD_UP; k <= BTN_DPAD_RIGHT && bidx < LIBJOY_MAX_BTN; k++) {
            if (test_bit(k, keybit)) { p->btn_map[k] = bidx++; }
        }
        p->nbtn = bidx;

        /* Absolute axes: map ABS_X/Y/etc to our axes array. ABS_HAT0X/Y go to a hat. */
        unsigned long absbit[NBITS(ABS_MAX)] = {0};
        if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) {
            int aidx = 0, hidx = 0;
            for (int a = 0; a < ABS_MAX; a++) {
                if (!test_bit(a, absbit)) continue;
                if (a == ABS_HAT0X || a == ABS_HAT0Y
                 || a == ABS_HAT1X || a == ABS_HAT1Y
                 || a == ABS_HAT2X || a == ABS_HAT2Y
                 || a == ABS_HAT3X || a == ABS_HAT3Y) {
                    int which_hat = (a - ABS_HAT0X) / 2;
                    if (which_hat < LIBJOY_MAX_HAT) {
                        p->abs_hat[a] = which_hat;
                        if (which_hat + 1 > p->nhat) p->nhat = which_hat + 1;
                    }
                    continue;
                }
                if (aidx >= LIBJOY_MAX_AXIS) continue;
                p->abs_map[a] = aidx++;
                struct input_absinfo info;
                if (ioctl(fd, EVIOCGABS(a), &info) >= 0) {
                    p->abs_min[a] = info.minimum;
                    p->abs_max[a] = info.maximum;
                    /* init to rest position scaled to SDL range */
                    int span = info.maximum - info.minimum;
                    int mid = (info.minimum + info.maximum) / 2;
                    int scaled = 0;
                    if (span > 0) scaled = ((info.value - mid) * 2 * 32767) / span;
                    if (scaled < -32768) scaled = -32768;
                    if (scaled > 32767) scaled = 32767;
                    p->axes[p->abs_map[a]] = scaled;
                }
            }
            p->naxis = aidx;
        }

        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        fprintf(stderr, "[JOY-RAW] opened #%d (%s) from %s — btn=%d axis=%d hat=%d\n",
            count, name, path, p->nbtn, p->naxis, p->nhat);
        fflush(stderr);
        count++;
    }
    return count;
}

static void libjoy_drain_pad(libjoy_rawpad_t *p) {
    if (p->fd < 0) return;
    struct input_event ev[32];
    ssize_t n;
    while ((n = read(p->fd, ev, sizeof(ev))) > 0) {
        int k = n / sizeof(ev[0]);
        for (int i = 0; i < k; i++) {
            struct input_event *e = &ev[i];
            if (e->type == EV_KEY && e->code < KEY_MAX) {
                int idx = p->btn_map[e->code];
                if (idx >= 0 && idx < LIBJOY_MAX_BTN) {
                    p->buttons[idx] = e->value ? 1 : 0;
                }
            } else if (e->type == EV_ABS && e->code < ABS_MAX) {
                if (p->abs_hat[e->code] >= 0) {
                    int h = p->abs_hat[e->code];
                    int is_x = ((e->code - ABS_HAT0X) & 1) == 0;
                    int cur = p->hats[h];
                    if (is_x) {
                        cur &= ~(SDL_HAT_LEFT | SDL_HAT_RIGHT);
                        if (e->value < 0) cur |= SDL_HAT_LEFT;
                        else if (e->value > 0) cur |= SDL_HAT_RIGHT;
                    } else {
                        cur &= ~(SDL_HAT_UP | SDL_HAT_DOWN);
                        if (e->value < 0) cur |= SDL_HAT_UP;
                        else if (e->value > 0) cur |= SDL_HAT_DOWN;
                    }
                    p->hats[h] = cur;
                } else {
                    int idx = p->abs_map[e->code];
                    if (idx >= 0) {
                        int mn = p->abs_min[e->code], mx = p->abs_max[e->code];
                        int span = mx - mn;
                        int mid = (mn + mx) / 2;
                        int scaled = 0;
                        if (span > 0) scaled = ((e->value - mid) * 2 * 32767) / span;
                        if (scaled < -32768) scaled = -32768;
                        if (scaled > 32767) scaled = 32767;
                        p->axes[idx] = scaled;
                    }
                }
            }
        }
    }
}
#endif /* LIBJOY_DIRECT_EVDEV */

/* --------------------------------------------------------------------------- */
/* libjoy_num ()                                                               */
/* Returns the number of joysticks present in the system                       */
/* --------------------------------------------------------------------------- */

int libjoy_num( void )
{
    return _max_joys ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_name (int JOY)                                                       */
/* Returns the name for a given joystick present in the system                 */
/* --------------------------------------------------------------------------- */

int libjoy_name( int joy )
{
    int result;
    result = string_new( SDL_GetJoystickNameForID( joy ) );
    string_use( result );
    return result;
}

/* --------------------------------------------------------------------------- */
/* libjoy_select (int JOY)                                                     */
/* Returns the selected joystick number                                        */
/* --------------------------------------------------------------------------- */

int libjoy_select( int joy )
{
    return ( _selected_joystick = joy );
}

/* --------------------------------------------------------------------------- */
/* libjoy_buttons ()                                                           */
/* Returns the selected joystick total buttons                                 */
/* --------------------------------------------------------------------------- */

int libjoy_buttons( void )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef TARGET_CAANOO
        if ( _selected_joystick == 0 ) return 21;
#endif
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) return _rawpads[_selected_joystick].nbtn;
#endif
        return SDL_GetNumJoystickButtons( _joysticks[ _selected_joystick ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_axes ()                                                              */
/* Returns the selected joystick total axes                                    */
/* --------------------------------------------------------------------------- */

int libjoy_axes( void )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) return _rawpads[_selected_joystick].naxis;
#endif
        return SDL_GetNumJoystickAxes( _joysticks[ _selected_joystick ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_button ( int button )                                            */
/* Returns the selected joystick state for the given button                    */
/* --------------------------------------------------------------------------- */

int libjoy_get_button( int button )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef TARGET_CAANOO
        if ( _selected_joystick == 0 )
        {
            int vax;

            switch ( button )
            {
                case    1: /* UPLF                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 );
                case    3: /* DWLF                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 );
                case    5: /* DWRT                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 );
                case    7: /* UPRT                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 );
                case    0: /* UP                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && ABS( vax ) < 16384 );
                case    4: /* DW                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && ABS( vax ) < 16384 );
                case    2: /* LF                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 && ABS( vax ) < 16384 );
                case    6: /* RT                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 && ABS( vax ) < 16384 );

                case    8:  /* MENU->HOME           */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 6 ) );
                case    9:  /* SELECT->HELP-II      */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 9 ) );
                case    10: /* L                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 4 ) );
                case    11: /* R                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 5 ) );
                case    12: /* A                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 0 ) );
                case    13: /* B                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 2 ) );
                case    14: /* X                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 1 ) );
                case    15: /* Y                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 3 ) );
                case    16: /* VOLUP                */  return ( 0 );
                case    17: /* VOLDOWN              */  return ( 0 );
                case    18: /* CLICK                */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 10 ) );
                case    19: /* POWER-LOCK  (CAANOO) */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 7 ) ); /* Only Caanoo */
                case    20: /* HELP-I      (CAANOO) */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 8 ) ); /* Only Caanoo */
                default:                                return ( 0 );
            }
        }
#endif
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) {
            if (button >= 0 && button < _rawpads[_selected_joystick].nbtn)
                return _rawpads[_selected_joystick].buttons[button];
            return 0;
        }
#endif
        return SDL_GetJoystickButton( _joysticks[ _selected_joystick ], button ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_position ( int axis )                                            */
/* Returns the selected joystick state for the given axis                      */
/* --------------------------------------------------------------------------- */

int libjoy_get_position( int axis )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) {
            if (axis >= 0 && axis < _rawpads[_selected_joystick].naxis)
                return _rawpads[_selected_joystick].axes[axis];
            return 0;
        }
#endif
        return SDL_GetJoystickAxis( _joysticks[ _selected_joystick ], axis ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_hats ()                                                              */
/* Returns the total number of POV hats of the current selected joystick       */
/* --------------------------------------------------------------------------- */

int libjoy_hats( void )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) return _rawpads[_selected_joystick].nhat;
#endif
        return SDL_GetNumJoystickHats( _joysticks[ _selected_joystick ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_balls ()                                                             */
/* Returns the total number of balls of the current selected joystick          */
/* --------------------------------------------------------------------------- */

int libjoy_balls( void )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
        return SDL_GetNumJoystickBalls( _joysticks[ _selected_joystick ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_hat (int HAT)                                                    */
/* Returns the state of the specfied hat on the current selected joystick      */
/* --------------------------------------------------------------------------- */

int libjoy_get_hat( int hat )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
#ifdef LIBJOY_DIRECT_EVDEV
        if (_rawpads[_selected_joystick].fd >= 0) {
            if (hat >= 0 && hat < _rawpads[_selected_joystick].nhat)
                return _rawpads[_selected_joystick].hats[hat];
            return SDL_HAT_CENTERED;
        }
#endif
        if ( hat >= 0 && hat <= SDL_GetNumJoystickHats( _joysticks[ _selected_joystick ] ) )
        {
            return SDL_GetJoystickHat( _joysticks[ _selected_joystick ], hat ) ;
        }
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_ball (int BALL, int* dx, int* dy)                                */
/* Returns the state of the specfied ball on the current selected joystick     */
/* --------------------------------------------------------------------------- */

int libjoy_get_ball( int ball, int * dx, int * dy )
{
    if ( _selected_joystick >= 0 && _selected_joystick < _max_joys )
    {
        if ( ball >= 0 && ball <= SDL_GetNumJoystickBalls( _joysticks[ball] ) )
        {
            return SDL_GetJoystickBall( _joysticks[ _selected_joystick ], ball, dx, dy ) ;
        }
    }
    return -1 ;
}

/* --------------------------------------------------------------------------- */

int libjoy_get_accel( int * x, int * y, int * z )
{
#ifdef TARGET_CAANOO
    if ( _selected_joystick == 0 )
    {
        KIONIX_ACCEL_read_LPF_g( x, y, z );
    }
    return 0;
#else
    return -1;
#endif
}

/* --------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------- */

/* --------------------------------------------------------------------------- */
/* libjoy_buttons_specific (int JOY)                                           */
/* Returns the selected joystick total buttons                                 */
/* --------------------------------------------------------------------------- */

int libjoy_buttons_specific( int joy )
{
    if ( joy >= 0 && joy < _max_joys )
    {
#ifdef TARGET_CAANOO
        if ( joy == 0 ) return 21;
#endif
        return SDL_GetNumJoystickButtons( _joysticks[ joy ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_axes_specific (int JOY)                                              */
/* Returns the selected joystick total axes                                    */
/* --------------------------------------------------------------------------- */

int libjoy_axes_specific( int joy )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        return SDL_GetNumJoystickAxes( _joysticks[ joy ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_button_specific (int JOY, int button)                            */
/* Returns the selected joystick state for the given button                    */
/* --------------------------------------------------------------------------- */

int libjoy_get_button_specific( int joy, int button )
{
    if ( joy >= 0 && joy < _max_joys )
    {
#ifdef TARGET_CAANOO
        if ( button >= 0 && ( ( joy == 0 && button <= 21 ) || ( joy != 0 && SDL_GetNumJoystickButtons( _joysticks[ joy ] ) ) ) )
#else
        if ( button >= 0 && button <= SDL_GetNumJoystickButtons( _joysticks[ joy ] ) )
#endif
        {
#ifdef TARGET_CAANOO
            if ( joy == 0 )
            {
                int vax;

                switch ( button )
                {
                    case    1: /* UPLF                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 );
                    case    3: /* DWLF                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 );
                    case    5: /* DWRT                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 );
                    case    7: /* UPRT                  */  return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 );
                    case    0: /* UP                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) < -16384 && ABS( vax ) < 16384 );
                    case    4: /* DW                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) >  16384 && ABS( vax ) < 16384 );
                    case    2: /* LF                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) < -16384 && ABS( vax ) < 16384 );
                    case    6: /* RT                    */  vax = SDL_GetJoystickAxis( _joysticks[ 0 ], 1 ) ; return ( SDL_GetJoystickAxis( _joysticks[ 0 ], 0 ) >  16384 && ABS( vax ) < 16384 );

                    case    8:  /* MENU->HOME           */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 6 ) );
                    case    9:  /* SELECT->HELP-II      */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 9 ) );
                    case    10: /* L                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 4 ) );
                    case    11: /* R                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 5 ) );
                    case    12: /* A                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 0 ) );
                    case    13: /* B                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 2 ) );
                    case    14: /* X                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 1 ) );
                    case    15: /* Y                    */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 3 ) );
                    case    16: /* VOLUP                */  return ( 0 );
                    case    17: /* VOLDOWN              */  return ( 0 );
                    case    18: /* CLICK                */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 10 ) );
                    case    19: /* POWER-LOCK  (CAANOO) */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 7 ) ); /* Only Caanoo */
                    case    20: /* HELP-I      (CAANOO) */  return ( SDL_GetJoystickButton( _joysticks[ 0 ], 8 ) ); /* Only Caanoo */
                    default:                                return ( 0 );
                }
            }
#endif
            return SDL_GetJoystickButton( _joysticks[ joy ], button ) ;
        }
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_position_specific (int JOY, int axis)                            */
/* Returns the selected joystick state for the given axis                      */
/* --------------------------------------------------------------------------- */

int libjoy_get_position_specific( int joy, int axis )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        if ( axis >= 0 && axis <= SDL_GetNumJoystickAxes( _joysticks[ joy ] ) )
        {
            return SDL_GetJoystickAxis( _joysticks[ joy ], axis ) ;
        }
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* Added by Sandman */
/* --------------------------------------------------------------------------- */
/* --------------------------------------------------------------------------- */
/* libjoy_hats_specific (int JOY)                                              */
/* Returns the total number of POV hats of the specified joystick              */
/* --------------------------------------------------------------------------- */

int libjoy_hats_specific( int joy )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        return SDL_GetNumJoystickHats( _joysticks[ joy ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_balls_specific (int JOY)                                             */
/* Returns the total number of balls of the specified joystick                 */
/* --------------------------------------------------------------------------- */

int libjoy_balls_specific( int joy )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        return SDL_GetNumJoystickBalls( _joysticks[ joy ] ) ;
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_hat_specific (int JOY, int HAT)                                  */
/* Returns the state of the specfied hat on the specified joystick             */
/* --------------------------------------------------------------------------- */

int libjoy_get_hat_specific( int joy, int hat )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        if ( hat >= 0 && hat <= SDL_GetNumJoystickHats( _joysticks[ joy ] ) )
        {
            return SDL_GetJoystickHat( _joysticks[ joy ], hat ) ;
        }
    }
    return 0 ;
}

/* --------------------------------------------------------------------------- */
/* libjoy_get_ball_specific (int JOY, int BALL, int* dx, int* dy)              */
/* Returns the state of the specfied ball on the specified joystick            */
/* --------------------------------------------------------------------------- */

int libjoy_get_ball_specific( int joy, int ball, int * dx, int * dy )
{
    if ( joy >= 0 && joy < _max_joys )
    {
        if ( ball >= 0 && ball <= SDL_GetNumJoystickBalls( _joysticks[ joy ] ) )
        {
            return SDL_GetJoystickBall( _joysticks[ joy ], ball, dx, dy ) ;
        }
    }
    return -1 ;
}

/* --------------------------------------------------------------------------- */

int libjoy_get_accel_specific( int joy, int * x, int * y, int * z )
{
#ifdef TARGET_CAANOO
    if ( joy == 0 )
    {
        KIONIX_ACCEL_read_LPF_g( x, y, z );
        return 0;
    }
#endif
    return -1;
}

/* --------------------------------------------------------------------------- */
/* Funciones de inicializacion del modulo/plugin                               */

DLCONSTANT __bgdexport( libjoy, constants_def )[] =
{
    { "JOY_HAT_CENTERED"    , TYPE_DWORD, SDL_HAT_CENTERED  },
    { "JOY_HAT_UP"          , TYPE_DWORD, SDL_HAT_UP        },
    { "JOY_HAT_RIGHT"       , TYPE_DWORD, SDL_HAT_RIGHT     },
    { "JOY_HAT_DOWN"        , TYPE_DWORD, SDL_HAT_DOWN      },
    { "JOY_HAT_LEFT"        , TYPE_DWORD, SDL_HAT_LEFT      },
    { "JOY_HAT_RIGHTUP"     , TYPE_DWORD, SDL_HAT_RIGHTUP   },
    { "JOY_HAT_RIGHTDOWN"   , TYPE_DWORD, SDL_HAT_RIGHTDOWN },
    { "JOY_HAT_LEFTUP"      , TYPE_DWORD, SDL_HAT_LEFTUP    },
    { "JOY_HAT_LEFTDOWN"    , TYPE_DWORD, SDL_HAT_LEFTDOWN  },
    { NULL                  , 0         , 0                 }
} ;

/* --------------------------------------------------------------------------- */

void  __bgdexport( libjoy, module_initialize )()
{
    int i;

#ifdef LIBJOY_DIRECT_EVDEV
    /* Zero-initialized statics have fd=0 (which is stdin!); mark unused. */
    for ( i = 0; i < MAX_JOYS; i++ ) _rawpads[i].fd = -1;
#endif

    /* BGD_DISABLE_SDL_JOYSTICK=1 opts out of opening joysticks entirely.
     * Useful when gptokeyb is the ONLY input path (e.g. a port that only
     * reads keyboard via libkey). Most bennugd games — including SORR —
     * actually poll libjoy directly via JOY_GETBUTTON/JOY_GETAXIS, so the
     * default MUST be "open the joystick". Previously this defaulted to
     * skipping on the 'mali' video driver (to avoid a theoretical race
     * with gptokeyb reading the same /dev/input/eventN), but that broke
     * every joystick-polling game on Amlogic FBDEV. The keyboard-path
     * race is now prevented by libsdlhandler's EVIOCGRAB, so there's no
     * reason to also block libjoy. */
    {
        const char *opt = SDL_getenv("BGD_DISABLE_SDL_JOYSTICK");
        if (opt && opt[0] != '0' && opt[0] != 'n' && opt[0] != 'N') {
            SDL_Log("libjoy: SDL joystick init skipped (BGD_DISABLE_SDL_JOYSTICK set)");
            _max_joys = 0;
            goto skip_joystick_init;
        }
    }

    if ( !SDL_WasInit( SDL_INIT_JOYSTICK ) )
    {
        SDL_InitSubSystem( SDL_INIT_JOYSTICK );
        SDL_SetJoystickEventsEnabled( true ) ;
    }

    /* SDL3: joystick enumeration is driven by udev and is ASYNC from
     * SDL_InitSubSystem. Calling SDL_GetJoysticks immediately after init
     * returns 0 devices because the first udev scan hasn't landed yet. */
    SDL_PumpEvents();
    SDL_Delay( 50 );
    SDL_PumpEvents();

#ifdef LIBJOY_DIRECT_EVDEV
    /* WORKAROUND: SDL3 lib32 (armhf) opens joystick fd but fails to drain
     * subsequent events — SDL_GetJoystickButton/Axis stays at open-time
     * values forever. Open the evdev devices ourselves and keep our own
     * state arrays; all libjoy_* getters prefer our cache when available. */
    {
        int n = libjoy_scan_pads();
        fprintf(stderr, "[JOY-RAW] direct evdev: %d gamepad(s) opened\n", n);
        fflush(stderr);
    }
#endif

    /* Open all joysticks (SDL3: enumerate by instance ID) */
    {
        int njoys = 0;
        SDL_JoystickID *ids = SDL_GetJoysticks( &njoys );
        fprintf(stderr, "[JOY] SDL_GetJoysticks = %d devices\n", njoys);
        fflush(stderr);
        if (( _max_joys = njoys ) > MAX_JOYS )
        {
            printf( "[JOY] Warning: maximum number of joysticks exceeded (%i>%i)", _max_joys, MAX_JOYS );
            _max_joys = MAX_JOYS;
        }
        for ( i = 0; i < _max_joys; i++ )
        {
            _joysticks[i] = SDL_OpenJoystick( ids[i] ) ;
            if ( !_joysticks[ i ] ) {
                fprintf(stderr, "[JOY] Failed to open joystick %d: %s\n", i, SDL_GetError());
                fflush(stderr);
            } else {
                int nbtn = SDL_GetNumJoystickButtons(_joysticks[i]);
                int nax  = SDL_GetNumJoystickAxes(_joysticks[i]);
                int nhat = SDL_GetNumJoystickHats(_joysticks[i]);
                fprintf(stderr, "[JOY] opened #%d: %s (buttons=%d axes=%d hats=%d)\n",
                    i, SDL_GetJoystickName(_joysticks[i]) ? SDL_GetJoystickName(_joysticks[i]) : "(null)",
                    nbtn, nax, nhat);
                fflush(stderr);
            }
        }
        SDL_free( ids );
    }

    SDL_UpdateJoysticks() ;

#ifdef LIBJOY_DIRECT_EVDEV
    /* If direct evdev opened any pads, expose them as _max_joys
     * regardless of what SDL reported. This way the game sees the
     * joysticks even when SDL3 lib32 fails to enumerate them. */
    {
        int raw_count = 0;
        for (int j = 0; j < MAX_JOYS; j++) if (_rawpads[j].fd >= 0) raw_count++;
        if (raw_count > _max_joys) _max_joys = raw_count;
    }
#endif

    /* Auto-select first available pad so games that don't call
     * joy_select() explicitly still see input. */
    if (_selected_joystick < 0 && _max_joys > 0) _selected_joystick = 0;

skip_joystick_init: ;

#ifdef TARGET_CAANOO
    KIONIX_ACCEL_init();

    if ( KIONIX_ACCEL_get_device_type() != DEVICE_TYPE_KIONIX_KXTF9 ) KIONIX_ACCEL_deinit();

    KXTF9_set_G_range(2);
    KXTF9_set_resolution(12);
    KXTF9_set_lpf_odr(400);

    KIONIX_ACCEL_enable_outputs();
#endif
}

/* ----------------------------------------------------------------- */

void  __bgdexport( libjoy, module_finalize )()
{
    int i;

#ifdef TARGET_CAANOO
    KIONIX_ACCEL_deinit();
#endif

    for ( i = 0; i < _max_joys; i++ )
        if ( _joysticks[ i ] ) SDL_CloseJoystick( _joysticks[ i ] ) ;

    if ( SDL_WasInit( SDL_INIT_JOYSTICK ) ) SDL_QuitSubSystem( SDL_INIT_JOYSTICK );

#ifdef LIBJOY_DIRECT_EVDEV
    for ( i = 0; i < MAX_JOYS; i++ ) {
        if (_rawpads[i].fd >= 0) { close(_rawpads[i].fd); _rawpads[i].fd = -1; }
    }
#endif
}

/* ----------------------------------------------------------------- */

/* DIAGNOSTIC: log the first joystick button press / axis movement we
 * see, per joystick. Useful for figuring out why a bennugd game feels
 * deaf to the gamepad — if this fires, the SDL → libjoy pipeline is
 * working and the game's own config is at fault; if it never fires,
 * the event loop or device open is broken. */
static void __libjoy_first_input_diag(void)
{
    /* Force SDL3 to re-read joystick fds every frame. On lib32 builds
     * SDL_PumpEvents alone doesn't seem to pick up evdev joystick events
     * reliably — explicitly kick SDL_UpdateJoysticks. */
    SDL_UpdateJoysticks();

#ifdef LIBJOY_DIRECT_EVDEV
    /* Drain raw evdev events into our own state arrays (workaround for
     * SDL3 lib32 not delivering joystick events). */
    for (int j = 0; j < MAX_JOYS; j++) {
        if (_rawpads[j].fd >= 0) libjoy_drain_pad(&_rawpads[j]);
    }
#endif

    static int first_btn_logged[16] = {0};
    static int first_axis_logged[16] = {0};
    static int tick = 0;
    int j;
    for (j = 0; j < _max_joys && j < 16; j++) {
        if (!_joysticks[j]) continue;
        int nbtn = SDL_GetNumJoystickButtons(_joysticks[j]);
        for (int b = 0; b < nbtn && !first_btn_logged[j]; b++) {
            if (SDL_GetJoystickButton(_joysticks[j], b)) {
                fprintf(stderr, "[JOY] first button press: joy#%d btn%d\n", j, b);
                fflush(stderr);
                first_btn_logged[j] = 1;
            }
        }
        int nax = SDL_GetNumJoystickAxes(_joysticks[j]);
        for (int a = 0; a < nax && !first_axis_logged[j]; a++) {
            int v = SDL_GetJoystickAxis(_joysticks[j], a);
            if (v > 16000 || v < -16000) {
                fprintf(stderr, "[JOY] first axis motion: joy#%d axis%d value=%d\n", j, a, v);
                fflush(stderr);
                first_axis_logged[j] = 1;
            }
        }
    }
    /* Dump rawpad state periodically — one line per open pad. */
    if ((tick++ % 60) == 0 && _max_joys > 0) {
#ifdef LIBJOY_DIRECT_EVDEV
        for (int j = 0; j < _max_joys; j++) {
            if (_rawpads[j].fd < 0) continue;
            int pc = 0;
            for (int b = 0; b < _rawpads[j].nbtn; b++) if (_rawpads[j].buttons[b]) pc++;
            fprintf(stderr, "[JOY-RAW] heartbeat joy#%d: pressed=%d/%d axis0=%d axis1=%d hat0=0x%x\n",
                j, pc, _rawpads[j].nbtn, _rawpads[j].axes[0], _rawpads[j].axes[1],
                _rawpads[j].nhat ? _rawpads[j].hats[0] : 0);
            fflush(stderr);
        }
#endif
    }
}

HOOK __bgdexport( libjoy, handler_hooks )[] =
{
    { 4800, __libjoy_first_input_diag },
    {    0, NULL }
};

char * __bgdexport( libjoy, modules_dependency )[] =
{
    "libsdlhandler",
    NULL
};

/* ----------------------------------------------------------------- */
