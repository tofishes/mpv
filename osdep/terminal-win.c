/* Windows TermIO
 *
 * copyright (C) 2003 Sascha Sommer
 *
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// See  http://msdn.microsoft.com/library/default.asp?url=/library/en-us/winui/WinUI/WindowsUserInterface/UserInput/VirtualKeyCodes.asp
// for additional virtual keycodes


#include "config.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <io.h>
#include "common/common.h"
#include "input/keycodes.h"
#include "input/input.h"
#include "terminal.h"
#include "osdep/io.h"
#include "osdep/w32_keyboard.h"

#define hSTDOUT GetStdHandle(STD_OUTPUT_HANDLE)
#define hSTDERR GetStdHandle(STD_ERROR_HANDLE)
static short stdoutAttrs = 0;
static const unsigned char ansi2win32[8] = {
    0,
    FOREGROUND_RED,
    FOREGROUND_GREEN,
    FOREGROUND_GREEN | FOREGROUND_RED,
    FOREGROUND_BLUE,
    FOREGROUND_BLUE  | FOREGROUND_RED,
    FOREGROUND_BLUE  | FOREGROUND_GREEN,
    FOREGROUND_BLUE  | FOREGROUND_GREEN | FOREGROUND_RED,
};

void terminal_get_size(int *w, int *h)
{
    CONSOLE_SCREEN_BUFFER_INFO cinfo;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cinfo)) {
        *w = cinfo.dwMaximumWindowSize.X - 1;
        *h = cinfo.dwMaximumWindowSize.Y;
    }
}

static int getch2_status = 0;

static int getch2_internal(void)
{
    DWORD retval;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    /*check if there are input events*/
    if (!GetNumberOfConsoleInputEvents(in, &retval))
        return -1;
    if (retval <= 0)
        return -1;

    /*read all events*/
    INPUT_RECORD eventbuffer[128];
    if (!ReadConsoleInput(in, eventbuffer, MP_ARRAY_SIZE(eventbuffer), &retval))
        return -1;

    /*filter out keyevents*/
    for (int i = 0; i < retval; i++) {
        switch (eventbuffer[i].EventType) {
        case KEY_EVENT: {
            KEY_EVENT_RECORD *record = &eventbuffer[i].Event.KeyEvent;

            /*only a pressed key is interresting for us*/
            if (record->bKeyDown) {
                UINT vkey = record->wVirtualKeyCode;
                bool ext = record->dwControlKeyState & ENHANCED_KEY;

                int mpkey = mp_w32_vkey_to_mpkey(vkey, ext);
                if (mpkey)
                    return mpkey;

                /*only characters should be remaining*/
                return eventbuffer[i].Event.KeyEvent.uChar.UnicodeChar;
            }
            break;
        }
        case MOUSE_EVENT:
        case WINDOW_BUFFER_SIZE_EVENT:
        case FOCUS_EVENT:
        case MENU_EVENT:
        default:
            break;
        }
    }
    return -1;
}

static bool getch2(struct input_ctx *ctx)
{
    int r = getch2_internal();
    if (r >= 0)
        mp_input_put_key(ctx, r);
    return true;
}

static int read_keys(void *ctx, int fd)
{
    if (getch2(ctx))
        return MP_INPUT_NOTHING;
    return MP_INPUT_DEAD;
}

void terminal_setup_getch(struct input_ctx *ictx)
{
    mp_input_add_fd(ictx, 0, 1, NULL, read_keys, NULL, ictx);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    getch2_status = !!GetNumberOfConsoleInputEvents(in, &(DWORD){0});
}

void getch2_poll(void)
{
}

void terminal_uninit(void)
{
    getch2_status = 0;
}

bool terminal_in_background(void)
{
    return false;
}

static void write_console_text(HANDLE *wstream, char *buf)
{
    wchar_t *out = mp_from_utf8(NULL, buf);
    size_t out_len = wcslen(out);
    WriteConsoleW(wstream, out, out_len, NULL, NULL);
    talloc_free(out);
}

// Mutates the input argument (buf), because we're evil.
void mp_write_console_ansi(HANDLE *wstream, char *buf)
{
    while (*buf) {
        char *next = strchr(buf, '\033');
        if (!next) {
            write_console_text(wstream, buf);
            break;
        }
        next[0] = '\0'; // mutate input for fun and profit
        write_console_text(wstream, buf);
        if (next[1] != '[') {
            write_console_text(wstream, "\033");
            buf = next;
            continue;
        }
        next += 2;
        // ANSI codes generally follow this syntax:
        //    "\033[" [ <i> (';' <i> )* ] <c>
        // where <i> are integers, and <c> a single char command code.
        // Also see: http://en.wikipedia.org/wiki/ANSI_escape_code#CSI_codes
        int params[2] = {-1, -1}; // 'm' might be unlimited; ignore that
        int num_params = 0;
        while (num_params < 2) {
            char *end = next;
            long p = strtol(next, &end, 10);
            if (end == next)
                break;
            next = end;
            params[num_params++] = p;
            if (next[0] != ';' || !next[0])
                break;
            next += 1;
        }
        char code = next[0];
        if (code)
            next += 1;
        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(wstream, &info);
        switch (code) {
        case 'K': {     // erase to end of line
            COORD at = info.dwCursorPosition;
            int len = info.dwSize.X - at.X;
            FillConsoleOutputCharacterW(wstream, ' ', len, at, &(DWORD){0});
            SetConsoleCursorPosition(wstream, at);
            break;
        }
        case 'A': {     // cursor up
            info.dwCursorPosition.Y -= 1;
            SetConsoleCursorPosition(wstream, info.dwCursorPosition);
            break;
        }
        case 'm': {     // "SGR"
            for (int n = 0; n < num_params; n++) {
                int p = params[n];
                if (p <= 0) {
                    SetConsoleTextAttribute(wstream, stdoutAttrs);
                } else if (p >= 0 && p < 8) {
                    SetConsoleTextAttribute(wstream,
                        ansi2win32[p] | FOREGROUND_INTENSITY);
                }
            }
            break;
        }
        }
        buf = next;
    }
}

int terminal_init(void)
{
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // We have been started by something with a console window.
        // Redirect output streams to that console's low-level handles,
        // so we can actually use WriteConsole later on.

        int hConHandle;

        hConHandle = _open_osfhandle((intptr_t)hSTDOUT, _O_TEXT);
        *stdout = *_fdopen(hConHandle, "w");
        setvbuf(stdout, NULL, _IONBF, 0);

        hConHandle = _open_osfhandle((intptr_t)hSTDERR, _O_TEXT);
        *stderr = *_fdopen(hConHandle, "w");
        setvbuf(stderr, NULL, _IONBF, 0);
    }

    CONSOLE_SCREEN_BUFFER_INFO cinfo;
    DWORD cmode = 0;
    GetConsoleMode(hSTDOUT, &cmode);
    cmode |= (ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);
    SetConsoleMode(hSTDOUT, cmode);
    SetConsoleMode(hSTDERR, cmode);
    GetConsoleScreenBufferInfo(hSTDOUT, &cinfo);
    stdoutAttrs = cinfo.wAttributes;
    return 0;
}
