/*  Copyright 2026 HandyAndy87

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

// The system log, and only the system log.
//
// Nothing in this plugin can log from the Miiverse applet process at all, so the
// counters in the config menu are the only place a result can be read.
//
// Reading it needs a debugger or an emulator, which is no help to anyone testing
// on a console, so I tried three times to also write the lines to the SD card.
// Every attempt hung the console: during plugin startup with the filesystem
// library uninitialized, then during plugin startup with a network socket, then
// at application start, which is still part of booting because the menu is itself
// an application. Do not reach for the filesystem from this plugin again without
// first pinning down a point in the lifecycle where it's genuinely safe.
//
// Diagnosing things without it works, just less comfortably: the failures this
// plugin produces can be told apart by their error codes, which is how every result
// so far was obtained.

// Off unless the user turns it on, at the foot of the Debug page as "Enable
// Debugger Logging".
//
// Every line here goes to the Cafe OS system log, which needs a debugger to read, so
// on an ordinary console the output gets written and never looked at. That was
// harmless while it was the only instrument. It stopped being obviously harmless
// when a console started taking twenty-five seconds to change title: an
// unconditional syscall on a path the system is waiting on is exactly the kind of
// thing that's cheap right up until it isn't. Off by default costs nothing to prove.
//
// This governs the log and nothing else now. The recent name list in the menu used
// to follow it too, which meant the one diagnostic a console owner can actually read
// was off by default behind a switch for output they can't read at all.
void log_set_enabled(bool enabled);
bool log_enabled();

void log_open();
void log_close();
void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define LOG(fmt, ...) log_line("[JustGetMiiOnline] " fmt, ##__VA_ARGS__)
