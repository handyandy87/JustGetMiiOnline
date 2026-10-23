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

// Installing the release the update check found, from the menu.
//
// When the check has found a newer release on this console's line, the root of the menu
// gets an "Install vX.Y.Z" item. Picking Yes and backing out toasts "Downloading
// JustGetMiiOnline vX.Y.Z" and hands the rest to a thread. It fetches the release's
// JustGetMiiOnline.wps.sha256, then JustGetMiiOnline.wps itself into
// JustGetMiiOnline.wps.part beside the installed plugin, hashing it as it arrives. Nothing
// is written over until the download is whole, matches the checksum and starts the way
// every Aroma plugin does. Then the installed file becomes JustGetMiiOnline.wps.bkup, which
// Aroma doesn't load, the new one takes its name, and the console relaunches.
//
// A release that can't go in this way gets a line instead of the item, pointing at the
// Releases page: one whose notes carry update.cpp's manual marker, one missing either
// file, or one whose notes were cut off before the marker could be ruled out.
//
// WHICH FILE.
//
// Aroma keeps the path each plugin was loaded from but doesn't hand it to plugins: the
// backend's API reports name, author, version and sections, not the source. So this
// replaces sd:/wiiu/environments/aroma/plugins/JustGetMiiOnline.wps, the name the README
// and every release use, and refuses when that file isn't there. Writing the new one
// beside a renamed copy would have Aroma load both.
//
// WHY A FULL RELAUNCH IS ENOUGH.
//
// WiiUPluginLoaderBackend reads the plugins folder only when it holds no plugins at all,
// which is at environment start, and runs a plugin's INITIALIZE_PLUGIN only for one it
// has just loaded. A full relaunch is what the settings already use, and it brings this
// plugin back with fresh state and a fresh INITIALIZE_PLUGIN, boot toast included. That
// only happens when the environment starts over, which reads the folder again and so
// loads the new file. No power cycle, and no backend library.
//
// It's made from the thread, once the files are in place, and only while the Wii U Menu
// holds the foreground: wiiload_plugin launches a title from its own receiver thread the
// same way. When the menu has handed the foreground to something, say the HOME Menu,
// the relaunch waits for the hook that gives it back. When the menu closes first,
// because something was started, there is no relaunching into somebody's game, so the
// toast asks for a restart instead.
//
// Toasts come from the hooks, as update.h has, except the first, which comes from the
// menu's close callback because that's where the choice is made.

// What the root of the menu shows for this. NONE adds nothing, ITEM is the Yes/No item
// with `text` as its label, and LINE is a stub carrying `text`.
enum class InstallOffer { NONE, ITEM, LINE };
InstallOffer update_install_offer(char *text, size_t size);

// The item's callback. Yes means install when the menu closes, and No takes it back.
void update_install_choose(bool yes);

// Call from the menu's close callback. Starts the install if the item was left on Yes,
// and answers whether it did. When it did, the install owns the relaunch: it relaunches
// when it's done, and does it even when the install fails if relaunch_after is set,
// which is how a setting changed in the same visit still gets its relaunch.
bool update_install_begin(bool relaunch_after);

// Call from ON_ACQUIRED_FOREGROUND and ON_RELEASE_FOREGROUND, so the relaunch only
// happens while the Wii U Menu is in front. Acquiring makes a waiting relaunch.
void update_install_foreground(bool acquired);

// Queues the failure or restart toast, once. Call from the same hooks as update_post.
void update_install_post();

// Call from ON_APPLICATION_ENDS. The thread is stopped and joined before the menu's
// process goes, as update.h sets out. A download running then is cut off, its .part
// removed and the installed plugin left alone, and the item is offered again next time.
// The swap is never cut off: its renames don't look at the stop flag, so the join waits
// them out, since the menu closing between them would leave no plugin at all. A relaunch
// still waiting after that becomes a request to restart.
//
// The relaunch the thread makes itself ends the menu too, and this join then waits for
// that same thread. That's not a thread waiting on itself: ON_APPLICATION_ENDS runs on the
// process's own thread as it exits, and SYSLaunchMenu only asks for the launch and
// returns, so the install thread finishes and the join returns. wiiload_plugin launches
// from its receiver thread and joins it at application end the same way.
// update_stop still checks for being called from the thread it would join.
void update_install_app_ends();

// The Debug page's "Update install: ..." line, without the label.
void update_install_status(char *out, size_t size);
