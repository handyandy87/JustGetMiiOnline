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

// Watch Miiverse's sign-in token request, and send it to the Miiverse's own account
// server on the two pairings where that isn't the selected one.
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
// THE ROUTED PAIRINGS
//
// Juxt and Protaverse each take their token from their own account server, so when
// either is paired with the other account server, Miiverse's request still goes to the
// real function, but with the account server lent to the Miiverse's own for the length
// of the call: Pretendo's for Juxt on Protarium, Protarium's for Protaverse on Pretendo.
// account.cpp does the lending and puts the selected server's address back afterwards,
// and choice.h has which pairings those are.
//
// ROSEVERSE
//
// Nothing here answers a request. Roseverse's token comes from Project Rose's RosePatcher
// plugin, installed alongside this one, which answers Miiverse's request itself in every
// process, the Miiverse applet included, and keeps its own account key. On Roseverse this
// hook hands the request on untouched and times it, which is how the Debug page tells a
// request RosePatcher answered from one that went to an account server.
//
// RosePatcher only installs that answer while its own Connect to Roséverse option is on,
// and once it's installed it answers whatever this plugin's setting says. On Protaverse
// or Juxt that hands the Miiverse a token it will refuse, and nothing here can get in
// front of it: whichever order the two patches went in, a request reaches theirs before
// it reaches nn_act. So that option has to be on for Roseverse and off for the other
// two, and rosepatcher.h has how this plugin keeps it that way.

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

// Lend the account server around the Miiverse applet's own token request on a routed
// pairing, by resolving nn_act's token function in the applet and patching the address it
// finds.
//
// This exists because the applet fetches its own token, in its own process, and a static
// replacement for the applet process never reaches the call: the applet loads nn_act after
// the backend applies its patches. Called from olv.cpp when the applet opens initial.oma,
// which is the one point the applet is known to be running. It installs on a routed
// pairing only, and pulls out any patch it left behind on every other setting, so calling
// it unconditionally from that file open is safe.
//
// The handler answers nothing. It lends the account server around the applet's own
// request, the way the Wii U Menu's and a game's requests are lent. It used to only watch
// that request, and an applet that asked before the Wii U Menu did got a token from the
// selected account server, which the Miiverse refused. token_applet_request_status() says
// what came of the last one.
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

// For the config menu, which is the only place any of this can be read. Counts
// rather than flags, for the reason olv.h gives: zero and two are different
// faults and a yes or no hides both.
//
// Routed is requests sent to the Miiverse's own account server, declined is requests that
// should have been and weren't, because the lend in account.cpp refused. The route line
// says what the last routed call returned, how long it took and what the caller asked for,
// and the time is what separates a token fetched from a server from one the console
// already held.
size_t token_requests_seen();
size_t token_requests_routed();
size_t token_routes_declined();
const char *token_route_status();

// For the config menu, on Roseverse. How many Miiverse token requests from the Wii U
// Menu or a game reached this plugin, and what the last one returned and how long it
// took.
//
// RosePatcher puts its answer in from its own application-start hook, which I expect to
// land after this plugin's static patch and so in front of it. Nothing has measured that
// order, and the readings cover both. "0 reached" once Miiverse has loaded means
// RosePatcher answered in front of this plugin. A request that did reach it and took
// seconds went to an account server, so RosePatcher wasn't answering, and Roseverse will
// refuse what came back. One that took a few milliseconds was answered behind this
// plugin, by RosePatcher or from act's own cache.
const char *token_roseverse_status();

// Which processes a request actually arrived in, and the id of the first one.
//
// This is here because a hardware run once reported no request at all while the applet
// hook in olv.cpp reported two visits in the same session. That rules out this plugin not
// running in the applet and leaves two candidates: the request happens somewhere I wasn't
// watching, or the replacement never installed. This names the first if that's what
// happened, and reading "nowhere" after a sign-in that plainly asked for a token is what
// says it's the second.
const char *token_where();
