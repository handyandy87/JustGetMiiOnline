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

#include "choice.h"

// One toast at boot naming the account server and Miiverse this plugin has selected,
// as in "Protarium + Protaverse Enabled".
//
// It exists because everything else here is invisible until something breaks. The
// console gives no sign that a Miiverse was moved, and the config menu is somewhere
// people go when they already suspect something. A line on screen at boot is the only
// moment the answer shows up without being asked for.
//
// The mechanism is Aroma's NotificationModule, reached through libnotifications, and it
// is a separate module a console may not have. Every call here checks its result, a
// console without it shows no toast, and the Debug page says how far it got. Nothing
// else changes.
//
// The first build of this never reached a console: Aroma's plugin loader refused the
// plugin before any hook ran. Nothing in this file was the reason. Linking
// libnotifications added one local symbol, which made the symbol table's count of them
// equal the index of .bss, and the loader reads whichever section's sh_info matches a
// loaded section as that section's relocations. The Makefile has the detail, and the
// check that stops it happening again.
//
// TIMING, AND WHY KEEP_UNTIL_SHOWN IS THE ONE THAT MATTERS.
//
// Three calls, in this order: set KEEP_UNTIL_SHOWN, set the fade-out to fifteen seconds,
// then add the notification. KEEP_UNTIL_SHOWN is the one that makes this work at boot at
// all. The overlay doesn't exist yet when a plugin initializes, so a notification posted
// then would be thrown away; that flag queues it until the overlay can show it, across
// application starts if need be. Nothing here times anything by hand.

// Post the toast. Call once from INITIALIZE_PLUGIN, after Config::Init has run, so the
// message names the settings this boot will actually use.
//
// ONCE PER BOOT, AND THE HOOK IS WHAT MAKES IT SO. Nothing in this file counts.
//
// Worth spelling out, because it's easy to assume otherwise: WUMS_APPLICATION_STARTS
// fires in the plugin backend on every title, so it looks as though every plugin hook
// must. INITIALIZE_PLUGIN does not. The backend calls the init hooks through a
// predicate, `!container.isInitDone()`, and stamps every container done immediately
// afterwards, so a plugin that is already loaded is skipped from the second title
// onward. ON_APPLICATION_START, called a few lines later with no predicate, is the one
// that runs every time.
//
// A plugin whose image is genuinely reloaded (the plugin set changed) does get a fresh
// INITIALIZE_PLUGIN, but its globals come back from disk with it, so no flag kept here
// could tell that apart from a boot. There is nothing for a guard to do either way,
// which is why there's no guard.
void notify_boot(Account account, Miiverse miiverse);

// Release the library. Call from DEINITIALIZE_PLUGIN. Does nothing unless the library
// was actually initialized, which isn't the same as notify_boot having run: a console
// with no NotificationModule reaches it and comes back with nothing held.
void notify_shutdown();

// What notify_boot did, for the Debug page. `queued` is the working reading, and means
// the module took the toast, not that the overlay has shown it yet. `init failed`,
// `defaults refused` and `refused` carry the code the call returned, a
// NotificationModuleStatus from libnotifications. `not run` means INITIALIZE_PLUGIN
// never reached it.
const char *notify_status();
