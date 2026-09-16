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

#include <cstdint>

// Zero means "leave everything exactly as Protarium's build left it" in both of
// these, so a missing or unreadable setting degrades to doing nothing at all. The
// whole plugin rests on that, so the numbering carries it. Anything added later
// goes on the end for the same reason: these numbers go into storage and get read
// back by a later build.

// Which account server signs the console's tokens. The first choice, because
// everything else follows from it: Miiverse only signs in against the service
// belonging to whichever server issued the token, and moving this moves
// matchmaking with it.
enum class Account : uint32_t {
    Protarium = 0,
    Pretendo = 1,
};

// Which Miiverse the console talks to.
//
// Protaverse is Protarium's own, and it's what their build already does. Picking it
// isn't a choice to write Protarium's addresses, it's a choice to write nothing.
enum class Miiverse : uint32_t {
    Protaverse = 0,
    Juxt = 1,
    Roseverse = 2,
};

// For the menu and the log, so the three names are spelled in one place rather
// than in a ternary at every site that prints one.
constexpr const char *miiverse_name(Miiverse miiverse) {
    switch (miiverse) {
        case Miiverse::Protaverse: return "Protaverse";
        case Miiverse::Juxt: return "Juxt";
        case Miiverse::Roseverse: return "Roseverse";
    }
    return "Protaverse";
}

// The Miiverse each account server signs into by itself.
//
// Measured, not assumed. Juxt with Protarium accounts fails with 115-5016, and the
// same console with only the account server moved to Pretendo signs in. So Protarium
// issues that token itself instead of asking Pretendo, and Juxt won't take one
// Pretendo didn't issue.
//
// This is the Miiverse a console with nothing stored starts on, and the menu's default
// for the item. An account switch doesn't move the Miiverse at all, which
// account_changed in config.cpp sets out. The other two pairings are selectable too,
// and miiverse_routed below says how each gets a token its Miiverse accepts.
constexpr Miiverse miiverse_for(Account account) {
    return account == Account::Pretendo ? Miiverse::Juxt : Miiverse::Protaverse;
}

// Whether Miiverse's token request has to go to the other account server.
//
// Juxt and Protaverse each take their token from their own account server. Juxt looks
// a token up in Pretendo's records and refuses any other, which is the 115-5016 above,
// and Protaverse is handed tokens only Protarium's account server can sign, with a key
// it holds. Paired with the other account server, each would get a token from the
// wrong issuer. So on those two pairings token.cpp lends the account server to the
// Miiverse's own for the length of that one request, and everything else stays where
// the setting put it.
//
// Roseverse is never routed. Its token is built on the console rather than asked for,
// so there's no issuer to be wrong.
//
// Protaverse on Pretendo stayed off the menu until its request could be routed, and
// what stood here then listed what offering it would take: the token request pointed
// at Protarium's account server, the name hooks in dns.cpp taught about Protaverse's
// hosts, and an answer for which SpotPass the pairing allows. The first is this and
// token.cpp, the second is the list by PROTARIUM_KEEP, and the third is the note by
// spotpass_valid below.
constexpr bool miiverse_routed(Account account, Miiverse miiverse) {
    return miiverse != Miiverse::Roseverse && miiverse != miiverse_for(account);
}

// Which network's SpotPass the console fetches from. The console calls it BOSS.
//
// This used to be derived from the two settings above instead of chosen, on the
// grounds that nobody has to decide it separately. That was right while every
// combination had one sensible answer. It stopped being right once Roseverse showed up
// with a SpotPass of its own, because then two combinations have two answers and each
// one forfeits something real: Protarium's carries Splatfests, Smash's Conquest and
// Mario Maker's 100 Mario Challenge, and Rose's carries the WaraWara Plaza belonging
// to the Miiverse actually being used. One server per console, so picking either one
// gives the other up. That's what makes this a setting and not a consequence.
//
// Zero is Protarium's, and Protarium's is what their build already points at, so a
// missing or unreadable value still means writing nothing.
enum class SpotPass : uint32_t {
    Protarium = 0,
    Pretendo = 1,
    Roseverse = 2,
};

constexpr const char *spotpass_name(SpotPass server) {
    switch (server) {
        case SpotPass::Protarium: return "Protarium";
        case SpotPass::Pretendo: return "Pretendo";
        case SpotPass::Roseverse: return "Roseverse";
    }
    return "Protarium";
}

// The account server's own SpotPass, always a legal answer. Tasksheets name content
// on the network that issued them, so whoever signs the console's tokens can always
// serve its own background work.
constexpr SpotPass spotpass_of(Account account) {
    return account == Account::Pretendo ? SpotPass::Pretendo : SpotPass::Protarium;
}

// Rose's SpotPass is on offer only when Roseverse is the Miiverse in use, for one
// reason: WaraWara Plaza is Miiverse's own and SpotPass feeds it. It has nothing to
// serve a console that isn't on Roseverse, so offering it there would be a position
// that does nothing.
//
// With Protarium as the account server it's also held back unless Advanced Overrides,
// near the foot of the Debug page, is on. A console on Protarium should use
// Protarium's SpotPass, which carries Splatfests, Smash's Conquest and Mario Maker's
// 100 Mario Challenge, and taking Rose's instead trades all that for a plaza. That's a
// trade for somebody who went looking for it, not a position to leave sitting in the
// menu inviting toggles. On Pretendo there's nothing of Protarium's left to give up,
// so Rose's is offered there like it always was.
constexpr bool spotpass_offers_roseverse(Account account, Miiverse miiverse, bool overrides) {
    return miiverse == Miiverse::Roseverse && (account == Account::Pretendo || overrides);
}

// The routed pairings get their account server's SpotPass and nothing else.
//
// For Protaverse on Pretendo that was decided, not overlooked, because Protarium's
// SpotPass is a real second answer there. Their tasksheets come from
// api.protarium.lol, which their discovery host also names as Protaverse's API, so
// theirs is what feeds Protaverse's WaraWara Plaza. It also carries their Splatfest
// and Conquest data, which would land on a console matchmaking on Pretendo's servers,
// and that's what settled it. The pairing keeps Pretendo's SpotPass the way Juxt on
// Protarium keeps Protarium's, and its WaraWara Plaza is fed from Pretendo's.
constexpr bool spotpass_valid(Account account, Miiverse miiverse, SpotPass chosen,
                              bool overrides) {
    if (chosen == spotpass_of(account)) {
        return true;
    }
    return chosen == SpotPass::Roseverse &&
           spotpass_offers_roseverse(account, miiverse, overrides);
}

// Which way the two-answer combinations lean, and they lean opposite ways on purpose.
//
// On Pretendo, Rose's is the default: nothing of Protarium's is being given up that
// choosing Pretendo didn't already give up, and the plaza should be served by whoever
// serves the Miiverse it belongs to. On Protarium, theirs is the default: picking a
// Miiverse isn't picking to give up Splatfests, and a default that took them away
// would be this plugin deciding that for somebody who asked for none of it.
//
// Advanced Overrides changes what's offered on Protarium and never this, so
// Protarium's default is Protarium's with the switch on or off.
constexpr SpotPass spotpass_default(Account account, Miiverse miiverse) {
    if (miiverse != Miiverse::Roseverse) {
        return spotpass_of(account);
    }
    return account == Account::Pretendo ? SpotPass::Roseverse : SpotPass::Protarium;
}
