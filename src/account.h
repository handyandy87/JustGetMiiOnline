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

#include <cstdint>

// The account server, which is one string inside IOSU.
//
// It lives here because Miiverse sign-in rides on a token that server signs, and
// Juxt checks tokens against Pretendo's own records rather than against a
// signature. So if Protarium's account server answers that request itself, Juxt
// refuses a token it has never seen, and this setting is the only way to tell
// that outcome apart from a swap that never landed.
//
// It's not a Miiverse setting. Move it and every online game moves with it,
// since those reach their servers through addresses the same server hands out.
// The menu says so.

void account_init();
void account_shutdown();

// Ask their module what it's up to. Different question from the one
// account_init() asks, and it has to be asked at a different time.
//
// account_init() reads the account URL out of IOSU, and that read is only good
// before this plugin has written to the same address, so it runs as early as
// there is. This one calls an export their module publishes, and that answer is
// only good after their plugin has had its turn to initialize the module, so it
// runs as late as an application start. Swap the two moments and both go wrong.
//
// Call once per application start, before account_apply(). It writes nothing and
// takes no setting: what it asks is true of the console, not of any choice made
// here.
void account_observe();

// Call once per application start. Writing repeatedly is harmless because the
// write compares first, and the default writes nothing at all.
void account_apply(Account chosen);

// The complaint to put at the top of the menu, or nullptr when there's none.
//
// A pointer rather than a bool, because there's more than one way for this
// plugin to have nothing to do and each one wants a different fix: no Inkay at
// all, an Inkay with its own network setting off, and an Inkay that's patching
// but isn't Protarium's. Calling all three "not found" sends somebody hunting
// for a file that's already installed.
//
// nullptr whenever nothing was positively determined, so a check that couldn't
// be made still reads as no complaint rather than as an accusation.
const char *account_warning();

// Which build the probe actually found, for the config menu. Three answers,
// because "could not check" is neither of the other two, and printing it as one
// asserts something this plugin doesn't know in the only readout there is.
const char *account_host_name();

// What their module says it's doing, for the config menu. Says nothing about
// whose build it is: every fork of Inkay exports this under the same name, so
// this line and the one above answer different halves of the question and both
// are worth printing.
const char *account_module_state();

// One line for the config menu, which is the only place a result can be read.
const char *account_status();

class IosuHandle;

// Point the account server at the other network for the length of one call, then put
// back exactly what was there. Only for the two routed pairings: Juxt on Protarium
// lends to Pretendo, Protaverse on Pretendo lends to Protarium. Called from the
// token hook.
//
// Both take the same handle, opened by the caller in its own process and held across
// the call in between, so the take-back never has to open one of its own. The lend
// refuses, and moves nothing, unless that handle is open and the address holds the
// selected account server's string: Protarium's before lending to Pretendo, and
// Pretendo's over the end of Protarium's before lending to Protarium.
// account_take_back() does nothing when nothing was lent, so a caller can fire it
// off blind.
bool account_lend(const IosuHandle &iosu, Account to);
void account_take_back(const IosuHandle &iosu);

// What the last lend did, for the config menu.
const char *account_lend_status();
