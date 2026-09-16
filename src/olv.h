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

// Point Miiverse at Juxt or at Roseverse, on top of Protarium's Inkay and without
// touching it.
//
// Protarium's module finds Nintendo's discovery URL and writes its own over it.
// This plugin runs second, finds what they wrote, and writes Juxt's over that.
// Their module stays installed, unmodified and in charge of everything else.
//
// Two rules keep the second write safe, and both are promises made here rather
// than properties of the mechanism. Break either one and this turns into the kind
// of patcher that wrecks the one it sits next to.
//
//   Never search for Nintendo's string. Their module needs it and consumes it.
//   Everything here keys on what Protarium leaves behind, which nobody else
//   wants, so there is no key to race over and nothing to take away.
//
//   Never write more bytes than were matched. Juxt's URL is longer than
//   Protarium's, which sounds like an overrun and isn't: the slot is the
//   39 bytes Nintendo's string occupied, Protarium's 31 bytes sit inside it, and
//   matching all 39 both proves the slot is that size and fills it exactly.
//   Roseverse's is 38 characters, the same as Nintendo's, so it fills the slot
//   with nothing left over at all.
//
// Two possible replacements now, and the key is still only ever Protarium's
// string, so the second rule is the same in substance and has only moved in where
// it gets proved. A replacement picked at runtime reaches swap_all as a
// const char *, which the compiler can't measure, so the widths are asserted
// equal at the declarations in olv.cpp instead of being obvious at the call site.
// Anything added there has to carry the same assertion.
//
// Nothing runs on the default. Picking Protaverse isn't a choice to write
// Protarium's own values back, it's a choice to write nothing, so it can't
// misfire and can't be told apart from this plugin not being there at all.

#include "choice.h"

#include <cstddef>
#include <cstdint>

// Snapshot the setting for the applet process, where storage cannot be read.
void olv_set_target(Miiverse target);

// Wii U Menu and games. Called when the process reaches the foreground, which is
// after their module has had its turn at application start. Acts once per title
// and only where the Miiverse library is actually loaded, because the foreground
// is taken again on every return from the HOME menu.
//
// WHY THERE IS NO APPLICATION-START TRIGGER, AND WHY ADDING ONE IS A TRAP.
//
// Neither trigger below is early enough. The Miiverse library reads the discovery
// URL before either one fires, so back when this plugin built Roseverse's token itself,
// a console pointed at Roseverse still looked up Protarium's host in the same process
// that had just taken a Roseverse token from this plugin. Writing after their module
// can't win a race that is already over.
//
// Claiming Nintendo's string at application start, before their module goes
// looking for it, does fix that. It was built, it worked, and it was removed,
// because of what it does to them. Taking their key costs them at every title
// start, for every title. Measured on hardware: five second transitions became
// forty, on apps with nothing to do with Miiverse.
//
// So from the plugin's own lifecycle hooks you can be before them, which costs
// them the scan, or after them, which is too late to matter. The gap between their
// write and the read is reached instead by the pre-main trigger at the bottom of
// this file, and that is what moves the Wii U Menu now. This trigger and the token
// one stick around for a process where the library wasn't loaded yet when that
// ran. Do not re-add an application-start claim here: the five-to-forty second
// transition cost measured above is what it would buy back.
void olv_apply_foreground();

// The same sweep, triggered instead by a Miiverse sign-in token being requested.
//
// The foreground turned out to be too late. A console pointed at Roseverse still
// resolved Protarium's discovery host, in the same process that had just taken a
// Roseverse token from this plugin when it still built them, so the URL was read before
// the foreground swap wrote over it. A token request is the earliest signal this plugin
// gets that the process is about to use Miiverse, rather than just that it came to the
// front. The once-per-title gate is shared, so whichever arrives first does the work.
//
// On Roseverse this may never fire now. RosePatcher answers the request, and when its
// patch sits outside mine the request never reaches this plugin at all. The pre-main
// trigger below doesn't depend on it.
void olv_apply_for_token();

// Both called at the start of every title, because both are per-process: the
// sweep has to happen again in the new process, and the callback list the new
// process owns is empty until something registers with it.
void reset_olv_sweep();
void olv_watch_late_loads();

// For the config menu, which is the only place any of this can be read. Counts
// rather than flags: zero and two are different faults, and a yes or no hides
// both.
size_t olv_applet_visits();
size_t olv_applet_url_swaps();
size_t olv_applet_allowlist_swaps();

// The applet's public suffix list, which only Roseverse needs. Three counts because
// they fail separately and each one says something different: no opens means the
// applet never read the file and the whole hook is in the wrong place, opens without
// reads means the handle tracking is wrong, and reads without rewrites means the file
// arrived in chunks that split the four bytes being looked for. olv.cpp explains that
// last one.
//
// RosePatcher rewrites the same four bytes in its own read hook whenever its Connect to
// Roséverse option is on, which Roseverse's token needs anyway. So with RosePatcher
// installed, reads without rewrites can also just mean its hook ran first and left
// nothing for this one to change.
size_t olv_tld_opens();
size_t olv_tld_reads();
size_t olv_tld_rewrites();
size_t olv_foreground_url_swaps();
size_t olv_token_trigger_swaps();

// Whether the sweep searched the library's own ranges or the whole of MEM2. The
// bounded path is both faster and more precise, and a console that can't answer
// where the library is falls back to the old behavior for the two later triggers
// and skips the early one entirely.
bool olv_sweep_bounded();

size_t olv_late_loads_seen();
size_t olv_late_load_swaps();

// THE ONE POINT BETWEEN THEIR WRITE AND THE LIBRARY'S READ
//
// Everything above races their module and loses in the Wii U Menu: hardware read
// Protaverse in process 2 with a sweep reporting success in that same process.
// Only the applet gets reached in time, which leaves the console split, with
// Miiverse on Roseverse and the menu's own Miiverse on Protaverse. Roseverse
// refuses that mixture.
//
// The handoff called the gap between their write and the library's read a window
// with no plugin hook in it. That was wrong. Their module writes from its
// all-starts-done hook; the loader then wipes the stack and calls one coreinit
// function, OSCheckActiveThreads, immediately before jumping to the title's own
// entry point. Replacing that function drops this plugin straight into the gap,
// whatever order the modules happened to load in.
//
// What it knows about Protarium is nothing at all. It keys on the same 39 bytes
// their module leaves behind that every other path here keys on, so a build of
// theirs that moves every offset still works as long as they still write a
// discovery URL. That is the whole reason this replaced an earlier attempt that
// recognized their build by its instructions: theirs is updated constantly, so any
// recognition like that is stale the day it ships.
//
// Armed once per title at application start, so the work happens at most once and
// the two triggers above become the no-ops that say so.
void olv_arm_premain();

// For the config menu. Runs and swaps separately, because a trigger that never
// fired and one that fired and found nothing want completely different things done
// about them, and the process id because that is what says which of the two the
// loader called it in.
size_t olv_premain_runs();
size_t olv_premain_swaps();
uint32_t olv_premain_pid();
