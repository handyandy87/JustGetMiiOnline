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

// Keep two of RosePatcher's own options in step with this plugin's Miiverse setting.
//
// RosePatcher supplies Roseverse's sign-in token and nothing else this plugin needs, but
// two of its options reach further than that and would fight the settings here:
//
//   Connect to Roséverse switches its token answer on, and once on it answers every
//   Miiverse sign-in on the console with Rose's token, which Protaverse and Juxt refuse.
//   It also moves a Pretendo discovery URL to Rose's, which is what Juxt writes. So it
//   has to be on for Roseverse and off for the other two.
//
//   Connect to Rosé News moves SpotPass to Rose by itself, over whatever the SpotPass
//   setting here chose. This plugin already writes Rose's SpotPass when that's what's
//   picked, so this one stays off.
//
// Its TVii options are left exactly as they are.
//
// HOW, AND WHY AT THAT MOMENT
//
// The plugin backend keeps each plugin's settings in memory, in a file per plugin on the
// SD card, and gives a plugin no way to reach another's. So this edits RosePatcher's file.
//
// When is the whole problem. RosePatcher reads its options once, when it loads at boot.
// And RosePatcher asks the backend to save every time the config menu closes, changed or
// not, and the backend writes RosePatcher's in-memory copy whenever it differs from the
// file. The backend calls every plugin's close callback in turn, in the order the plugins
// sit in the folder, so an edit made anywhere in the menu, this plugin's own close
// callback included, can be written straight back over by RosePatcher's close callback a
// moment later.
//
// The one moment nothing can write it back is after the menu has closed, while the
// application is ending for the relaunch the menu asked for. That relaunch initializes
// every plugin again, which is measured for this plugin's own settings, and a plugin the
// backend loads fresh has its settings read from the SD card, so RosePatcher should read
// what was written here. So the menu only reads, and the write waits for
// ON_APPLICATION_ENDS. Nothing has measured a file write at that point in the lifecycle
// yet. "RosePatcher options: in step" on the Debug page after the relaunch is the reading
// that says it worked, and the same options still out of step is the one that says the
// backend wrote RosePatcher's own copy back over them.

// Read RosePatcher's options and note whether they match what `miiverse` wants. Called
// when the config menu opens, the same point in the lifecycle this plugin read a file on
// the SD card from on hardware before. Reads only.
void rosepatcher_check(Miiverse miiverse);

// What the last check found. Found means RosePatcher's file exists and holds both
// options. Out of step means at least one of them doesn't match yet.
bool rosepatcher_found();
bool rosepatcher_out_of_step();

// Ask for the options to be set when the current application ends. Called from the
// menu's close callback just before it relaunches the console, and only then, because
// only a relaunch makes RosePatcher read what gets written.
void rosepatcher_arm_sync();

// Do the write, if one was asked for. Called from ON_APPLICATION_ENDS, and a no-op on every
// other application end. Rereads the file first, because RosePatcher may have saved a
// change of its own when the menu closed, and writes nothing when the options already
// match.
void rosepatcher_sync(Miiverse miiverse);

// One line for the Debug page.
const char *rosepatcher_status();
