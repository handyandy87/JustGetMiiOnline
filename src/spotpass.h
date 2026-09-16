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

// Move SpotPass, which the console calls BOSS, to the network the user picked.
//
// SpotPass is the background service that fetches things while the console idles, and
// WaraWara Plaza gets fed this way. Which network serves it used to be derived here
// from the account server and the Miiverse. It's a setting now, because the two
// Roseverse pairings have two defensible answers and each one gives the other up:
// choice.h holds which values a pairing allows and which way it leans, including the
// Advanced Overrides switch that holds one back on Protarium, and this file holds only
// what each destination means in addresses.
//
// Three destinations:
//
//   Protarium: write nothing at all. Their build already points every one of these
//   four addresses where it wants them, so their own SpotPass is what a console gets
//   by doing nothing, and that's what makes this position indistinguishable from the
//   plugin not being installed. It's also why it's the default wherever Protarium is
//   the account server: their SpotPass carries Splatfests, Smash's Conquest and Mario
//   Maker's 100 Mario Challenge, and a default that took those away would be this
//   plugin deciding something nobody asked it to decide.
//
//   Pretendo: the two tasksheet slots move to Pretendo.
//
//   Roseverse: all four move to Rose. Reachable from either account server, though
//   from Protarium only with Advanced Overrides on. Nothing about the addresses
//   changes with the account setting: the tasksheet slots hold Protarium's strings
//   either way, and the policy pair holds Pretendo's, because that's what Protarium's
//   module leaves in them.
//
// How many addresses move differs between the last two, and that's Protarium's doing
// rather than a choice here.
//
//   Pretendo moves two. Protarium's build points only the two tasksheet slots at
//   itself and leaves the BOSS policy pair on Pretendo already, so the policy list and
//   the policy host already hold what I'd write, before anything here runs. Writing
//   a value that's already there buys nothing and adds two more places to get wrong.
//
//   Roseverse moves four, for that same reason read the other way. Leaving the policy
//   pair on Pretendo while taking tasksheets from Rose is precisely the "one network
//   for work the other issued" condition this file objects to.
//
// Both extra writes are exactly as long as what they match, which puts them in a
// stronger spot than the tasksheets rather than a weaker one: there's no room
// argument to get wrong, and a full-width match of the expected incumbent is the only
// proof available that the address is right on this console.
//
// Call once per application start. Calling it again is harmless, since every write
// compares first, and on the Protarium setting nothing is written at all.
void spotpass_apply(SpotPass chosen);

// Read all four addresses and record which host each one actually holds.
//
// This is the difference between reporting a setting and reporting a console. The
// status lines below say what this plugin did, which is worth knowing and isn't the
// same question: a slot can hold Rose's host because this wrote it, because a previous
// boot did, or because some other build did, and only a read tells the three apart. It
// is also the only line that stays honest when a setting was changed and the reboot
// that applies it hasn't happened yet.
//
// Reads only, so it's safe to call from the config menu, which is where it's called
// from, and it therefore reports the state at the moment the menu was opened.
void spotpass_observe();

// The host found at each address, as a hostname rather than a network name.
//
// A hostname because the policy pair cannot be labeled honestly any other way:
// Protarium's module leaves Pretendo's own strings in those two, so "Pretendo" there
// means both "Rose's was not written" and "Pretendo's is what is there", and a reader
// can't tell which was meant. The host itself has no such ambiguity.
//
// "no IOSU" when Mocha isn't available at all, "not readable yet at" and the address
// when IOSU wouldn't answer for that address, which is normal early in a cold boot,
// and "not recognized" when it answered with something none of the three builds
// writes.
const char *spotpass_long_seen();
const char *spotpass_short_seen();
const char *spotpass_policy_seen();
const char *spotpass_policy_host_seen();

// One line per slot for the config menu, which is the only place a result can be read
// on this console.
//
// These say more than whether it worked, because one step of this rests on an
// assumption nothing in the repo can settle: that the tail of Nintendo's original
// string is still sitting past Protarium's terminator, which is what proves the slot
// is wide enough for Pretendo's longer URL. If that's wrong the write is refused
// rather than attempted, and these report the bytes actually found, so the console
// itself says what's there.
const char *spotpass_long_status();
const char *spotpass_short_status();

// The BOSS policy pair, which only Roseverse moves. Reported separately from the
// tasksheets and from each other for the same reason the tasksheets are split up: each
// stands on its own guard, so one refusing is a different fault from all four refusing
// and a single line would hide it.
//
// These two have never been read on a console. "not theirs" is what they say if the
// address or the expected contents turn out to be wrong, and nothing is written when
// they say it.
const char *spotpass_policy_status();
const char *spotpass_policy_host_status();

// Whether this combination moves the policy pair at all, so the menu can drop the two
// lines entirely instead of printing "left alone" twice on every console that will
// never move them. The destination rule stays in spotpass.cpp; this is the one
// question the menu needs to ask about it.
bool spotpass_moves_policy(SpotPass chosen);
