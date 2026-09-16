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

// Turn off Protarium's game redirects when the account server is Pretendo.
//
// Their build sends five games to their own hosts by name: Wii Karaoke U,
// Monster Hunter 3 Ultimate, Black Ops II, and, since their r10 build, Just
// Dance and Lost Reavers. Those hosts expect a token their account server
// issued, so on Pretendo the redirect drops the game on a server that won't
// have it. Worse than useless there: it swaps "this game is not served" for a
// connection that opens and then fails.
//
// Their two friend-server redirects are the opposite case and are never touched.
// They point at pretendo.cc, which is exactly what a console on Pretendo needs.
//
// The games aren't listed by name any more, and that's the point. Their build
// gains games regularly, so a list of the hosts they redirect to was stale the day
// it was written and left exactly the names nobody predicted still redirected.
// Everything on their domain is refused by rule instead, so a game they ship
// tomorrow is covered the day they ship it.
//
// Four names are kept: their account server, and Protaverse's discovery host, API
// host and portal, the API host being their SpotPass host as well. Protaverse's are
// kept because it's offered on Pretendo, with its token request routed to their
// account server, so a setting here selects them. The account server is kept because
// it's the subject of a write rather than of a refusal: that decision is made by
// moving a string, this file would only be enforcing it a second time and more
// bluntly, and anything later that rewrites where a lookup goes needs the name to
// arrive here in the first place.
//
// The hooks install themselves through declarations in dns.cpp, so there's no
// setup call beyond handing over the setting.

#include "choice.h"

#include <cstddef>

// Snapshot the setting, the way olv.cpp does, rather than reading storage from
// inside a name lookup.
//
// Called only from Config::Init, which runs when the plugin is initialized: once
// per boot, before the first title resolves anything, and not once per title.
// account.cpp has the detail, by PROTARIUM_MARKER. Deliberately not called from
// the menu callback: that would move this third of the account setting mid-session
// while the two that write to the security processor waited for a reboot, and a
// console with the name redirects off and the account address still pointing the
// other way isn't a configuration anybody chose. A changed setting arrives here
// with the relaunch the menu forces when it closes.
void dns_set_account(Account chosen);

// Start each title's counters over, so a launch reports its own lookups rather
// than the first launch after a reboot.
void reset_dns_counters();

// Every Miiverse discovery host that was actually resolved, and the process each
// was first seen in. Never reset, because the process that answers this is one the
// config menu can't be opened from.
const char *dns_discovery_seen();

// How many lookups were seen and how many were refused. Counts rather than a
// flag: zero refusals with a game running is a different fault from the hook not
// running at all, and both are invisible in a yes or no.
size_t dns_lookups_seen();
size_t dns_lookups_refused();

// Which way round the two plugins' hooks ended up nested, read off the only thing
// that can say: the spelling of the name that arrived.
//
// A name still spelled the way the game asked means this hook runs before
// Protarium's and theirs hasn't rewritten it yet. A name already spelled
// protarium.lol means theirs ran first and this one sees what they produced. The
// refusal above covers both, so nothing depends on the answer, but it's the one
// reading that explains a surprise, and it costs nothing to report.
const char *dns_nesting();

// The last few names verbatim, oldest first, with whether each was refused. The
// only way to see a name that reaches Protarium's redirect and not this one.
size_t dns_recent_count();
const char *dns_recent(size_t index);
bool dns_recent_refused(size_t index);
