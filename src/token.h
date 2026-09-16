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

#include <cstddef>

// Answer Miiverse's sign-in token request on the console instead of asking an
// account server for one.
//
// Third of the three things a move to Roseverse needs, and the one this plugin
// didn't do until now. The other two are a string swap each. This one answers a call
// rather than declining one, which is why it's the only hook here that hands back a
// value I made up.
//
// WHAT IT INTERCEPTS, AND WHAT IT DELIBERATELY DOES NOT
//
// nn::act::AcquireIndependentServiceToken is how every online service on the
// console gets a token, not just Miiverse. My hook checks the service being asked for
// against one client id and passes everything else straight through to the real
// function, so games keep getting real tokens from whichever account server is
// selected. That pass-through is the part to keep if anything here ever changes:
// widen the match and you take the whole console's online access with it.
//
// WHERE THE CONSTANT COMES FROM
//
// The constant is imported from the user's own copy of Rose's module and is never in
// this source. See secret.h.
//
// WHAT IT WILL NOT DO
//
// It never writes to the SD card, and it never generates a per-account key when it
// finds none: writing one would risk clobbering a key a working Roseverse setup
// already depends on, and all three hangs recorded in logger.h involved this plugin
// reaching for the filesystem. Reading a key that's already there is the most I need,
// which is what the README has said since before any of this was built. With no key
// the hook declines, and the console behaves as it does today.
//
// THE ROUTED PAIRINGS
//
// The same hook has one more job, and it answers nothing. Juxt and Protaverse each
// take their token from their own account server, so when either is paired with the
// other account server, Miiverse's request still goes to the real function, but with
// the account server lent to the Miiverse's own for the length of the call:
// Pretendo's for Juxt on Protarium, Protarium's for Protaverse on Pretendo.
// account.cpp does the lending and puts the selected server's address back
// afterwards, and choice.h has which pairings those are.

// Snapshot the setting for the Miiverse applet, where storage cannot be read.
// Same mechanism and same reason as olv_set_target.
void token_set_target(Miiverse target);

// The account server, snapshotted the same way. Called from Config::Init only: a
// changed account server arrives with the relaunch the menu forces, as everything
// else it moves does.
void token_set_account(Account account);

// Initialize the runtime function patcher, once per boot. Called from
// INITIALIZE_PLUGIN, because the applet install below needs it and the applet is a
// process this plugin gets no lifecycle hook in.
void token_init();

// Build the Miiverse token before anything can ask for it. Called at every
// application start, and a no-op unless Roseverse is selected and the cache is either
// empty or built for a different account.
//
// This exists because the applet can't build one, and on a cold boot straight into
// Miiverse the applet is the first thing to ask. Measured: the request arrived in
// process 9 with nothing cached, was passed through, and Roseverse refused the real
// token that came back. Only the Wii U Menu or a game can warm the cache, so one of
// them has to do it before the applet gets there.
void token_warm_cache();

// Install the token answer inside the Miiverse applet, by resolving nn_act's token
// function in the applet and patching the address it finds.
//
// This exists because the applet fetches its own token, in its own process, and the
// static replacement for the applet process never reaches the call: the applet loads
// nn_act after the backend applies its patches. Called from olv.cpp when the applet
// opens initial.oma, which is the one point the applet is known to be running. It
// installs on Roseverse and on a routed pairing, and pulls out any patch it left
// behind on every other setting, so calling it unconditionally from that file open is
// safe.
//
// On a routed pairing the handler answers nothing. It lends the account server around the
// applet's own request, the way the Wii U Menu's and a game's requests are lent. It used to
// only watch that request, and an applet that asked before the Wii U Menu did got a token
// from the selected account server, which the Miiverse refused.
// token_applet_request_status() says what came of the last one.
void token_apply_applet_hook();

// For the config menu. One line saying what the last applet install attempt did,
// because a request that never reaches the applet and one the applet declined want
// different things looked at. Written in the applet, read in the menu, with a plugin
// global carrying it between the two.
const char *token_applet_status();

// For the config menu, on a routed pairing. The applet's own last request: the code act
// returned, how long it took, the cache duration the applet asked for with "sent 0" when a
// fresh token was forced instead, whether the account server was lent, and the flags. A few
// milliseconds means act handed back a token it held, which only follows a routed fetch
// that already came back this boot. Seconds with "lent" is a fresh fetch from the
// Miiverse's own account server. "not lent" means the lend refused, and the request went
// to the selected account server, the token the route exists to avoid.
const char *token_applet_request_status();

// The same install for the in-game Miiverse overlay, process 7.
//
// Measured before it existed: `nn_act pid 7 ... untouched`, so the registration by
// name aimed at that process never took hold, exactly as it never did in the applet.
// What I inferred from that, and shouldn't have, is that the overlay was therefore
// asking the real account server and being refused. It isn't asking at all. Posting
// from inside a game on Roseverse works with this installed and with no `overlay` in
// `Token seen in`, meaning the handler never ran: the game fetches the token and the
// overlay uses what the game already holds. So this install is dormant on every
// reading so far, and I keep it for a title whose overlay does ask, if one exists.
// One game has been tried.
//
// Installed by address from the overlay's own file opens, retried until it takes
// because that process has no landmark file the way the applet has initial.oma.
//
// Dropped again at every title start, so each overlay session installs into its own
// process rather than trusting a patch added in a previous one.
void token_apply_overlay_hook();
void token_reset_overlay_hook();

// For the config menu. "installed in pid 7" says the patch went in. `overlay` in the
// line above would say the overlay actually asked for a token, and it has never once
// appeared, which is why I read this install as dormant rather than working.
const char *token_overlay_status();

// For the config menu, which is the only place any of this can be read. Counts
// rather than flags, for the reason olv.h gives: zero and two are different
// faults and a yes or no hides both.
size_t token_requests_seen();
size_t token_requests_answered();
size_t token_requests_passed();

// The routed pairings. Routed is requests sent to the Miiverse's own account server,
// declined is requests that should have been and weren't, because the lend in
// account.cpp refused. The route line says what the last routed call returned, how
// long it took and what the caller asked for, and the time is what separates a token
// fetched from a server from one the console already held.
size_t token_requests_routed();
size_t token_routes_declined();

// How many times the console loaded an account while I was watching.
//
// The hook behind this rebuilds the token, because an account having just loaded is
// the one moment every field the token is built from is known to be true. See the
// note by the hook in token.cpp: I got it wrong twice, once by marking the token
// stale and once by doing nothing at all, and both failures are written up there.
size_t token_account_loads();

// Which moment produced the token currently cached, and in which process.
//
// A token built before the account finished loading is well formed and wrong. On
// hardware it made Roseverse behave as though the console had no account set up,
// while the state line still read "token built" and the counters still read answered.
// Nothing else here can tell those apart, which is why this exists. "app start"
// beside a launch that failed that way is the reading to look for; "account load" is
// the one that should be standing by the time the applet asks.
const char *token_build_origin();

// Record where nn_act's token function sits in the calling process. Called at every
// application start, from the applet's own file open, and once from the in-game
// overlay, so the three can be compared.
void token_note_act_address();

// Those sightings, one process per call, with the count to walk them.
//
// Measured on hardware: every process has nn_act at its own address. A game read
// 0f85a948, the Wii U Menu 0e44e488 and the applet 0db24c08, megabytes apart. One
// thing follows.
//
// It's not why a registration by name misses the applet. Those registrations carry
// no address, and the loader resolves the name in whichever process it's patching,
// so a different address per process can't break them. That leaves the other
// candidate: nn_act wasn't loaded when the backend applied its patches, and nothing
// re-applies them afterwards.
//
// Each entry also carries the instruction found at that address, which says whether
// anything had already patched the function in that process. That's what answers the
// standing question about the in-game overlay: whether the registration aimed at it
// ever takes hold, without needing the overlay to ask for a token first.
size_t token_act_sighting_count();
const char *token_act_sighting(size_t index);
const char *token_route_status();

// One line saying what the last attempt to build a token did, because a count of
// zero answered has several causes and they need different things done.
const char *token_status();

// Which processes a request actually arrived in, and the id of the first one.
//
// This is here because my first hardware run reported no request at all while the
// applet hook in olv.cpp reported two visits in the same session. That rules out this
// plugin not running in the applet and leaves two candidates: the request happens
// somewhere I wasn't watching, or the replacement never installed. This names the
// first if that's what happened, and reading "nowhere" after a sign-in that plainly
// asked for a token is what says it's the second.
const char *token_where();
