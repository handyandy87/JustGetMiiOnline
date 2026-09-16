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

#include <cstddef>

// Clear the Wii U Menu's WaraWara Plaza so it refills from whichever Miiverse is
// actually selected.
//
// The plaza is fed by a SpotPass task called "oltopic", registered once per network
// account. Nothing re-points a task that is already registered, so a console that
// switches Miiverse keeps showing the old service's plaza forever. The content is
// not stale in any way the console can notice, it's just the answer a different
// server gave. Unregistering the task is what makes the console register it again
// on the next boot, against whichever service is configured by then.
//
// So this can't take effect in place, and that's why the menu pairs it with the
// relaunch: the reset is the unregistration, the refill happens at the next boot. An
// action that looked like it did nothing until some later reboot would be
// indistinguishable from one that failed.

// Whether the action can run at all.
//
// False outside the Wii U Menu. I haven't established why that restriction is
// needed, and nothing here has measured it. The menu shows the line instead of the
// item, so nobody is left pressing a control that quietly does nothing.
bool plaza_available();

// Unregister the task for every signed-in network account, and return how many
// accounts that was.
//
// Zero is a normal answer, not a failure: a console with only local accounts has
// no task registered to remove. Callers use the count to decide whether forcing a
// relaunch is worth it, so the difference matters.
//
// Local accounts get skipped rather than attempted. The task is keyed on a
// persistent id that only a network account has.
size_t plaza_reset();

// One line for the config menu, which is the only place a result can be read. Says
// what the last attempt did, or why the action isn't on offer.
const char *plaza_status();
