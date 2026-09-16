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

#include "token.h"
#include "account.h"
#include "iosu.h"
#include "olv.h"

#include <wups.h>

#include <function_patcher/function_patching.h>

#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/memorymap.h>
#include <coreinit/time.h>

#include <cstdio>
#include <cstring>

namespace {

// The one service I route. Everything else passes through untouched, and that isn't
// optional: this symbol is how every online service on the console gets a token, so
// widening the match takes the whole console with it.
constexpr const char *MIIVERSE_CLIENT_ID = "87cd32617f1985439ea608c2746e4610";

Miiverse target = Miiverse::Protaverse;

// The account server, snapshotted the same way and for the same reason as the Miiverse
// above. It takes both snapshots to know whether I'm looking at a routed pairing.
Account account = Account::Protarium;

size_t requests_seen = 0;
size_t requests_routed = 0;
size_t routes_declined = 0;

// The last routed call, for the Debug page.
//
// I keep what the caller asked for beside what came back. The cache duration is the one
// argument a routed call changes, and nothing has measured what the console's own callers
// pass in it. How long the call took is what says whether act went to a server at all or
// handed back a token it already held.
bool have_route = false;
int32_t route_result = 0;
uint32_t route_ms = 0;
uint32_t route_cache_asked = 0;
bool route_flag_a = false;
bool route_flag_b = false;
char route_line[96];

// The last Miiverse request from the Wii U Menu or a game on Roseverse that got as far as
// this plugin, kept the same way. RosePatcher is meant to answer these, so each one that
// arrives here is worth timing: see token_roseverse_status in token.h for the reading.
size_t roseverse_reached = 0;
bool have_roseverse_call = false;
int32_t roseverse_result = 0;
uint32_t roseverse_ms = 0;
char roseverse_line[96];

// The applet's own last request on a routed pairing, kept the same way for the same page:
// what came back, how long it took, the cache duration the applet asked for and the one
// actually sent, and whether the account server was lent for it. See route_applet().
bool have_applet_call = false;
int32_t applet_result = 0;
uint32_t applet_ms = 0;
uint32_t applet_cache_asked = 0;
uint32_t applet_cache_sent = 0;
bool applet_lent = false;
bool applet_flag_a = false;
bool applet_flag_b = false;
char applet_request_line[96];

// Whether a routed fetch has come back with a token since this plugin started, in any
// process. Until one has, act may still be holding a Miiverse token the selected account
// server issued before a relaunch changed the setting, so the applet's own routed request
// asks for a fresh one instead of taking whatever act holds.
bool routed_token_fresh = false;

// Which process the request arrived in, one bit each, plus the process id of the first
// one to show up.
//
// This exists because a hardware run once reported zero requests seen while the applet
// hook in olv.cpp reported two visits in the same session. That rules out this plugin not
// running in the applet and leaves two possibilities: the request happens in a process I
// wasn't watching, or the replacement never installed. One bit per target separates them.
// Any bit set means the first; no bit set with a request that plainly happened means the
// second.
uint32_t fired_mask = 0;
uint32_t first_upid = 0;

constexpr uint32_t FIRED_MIIVERSE = 1u << 0;
constexpr uint32_t FIRED_GAME_MENU = 1u << 1;
constexpr uint32_t FIRED_HOME = 1u << 2;
constexpr uint32_t FIRED_BROWSER = 1u << 3;

char where[96];

void note_process(uint32_t bit) {
    if (fired_mask == 0) {
        first_upid = static_cast<uint32_t>(OSGetUPID());
    }
    fired_mask |= bit;
}

bool is_miiverse(const char *client_id) {
    return client_id != nullptr && std::strcmp(client_id, MIIVERSE_CLIENT_ID) == 0;
}

// What the Wii U Menu and game wrappers below do with a request. Three outcomes rather
// than two, because Roseverse neither routes a request nor leaves it entirely alone.
enum class Reply {
    Pass,
    Route,
    Watch,
};

// Written once and called from every process wrapper below, which differ only in which
// real function they fall through to.
Reply decide(const char *client_id) {
    ++requests_seen;

    if (!is_miiverse(client_id)) {
        return Reply::Pass;
    }

    // First thing, and deliberately not gated on the pairing.
    //
    // A request for this client id is my earliest warning that the process is about to
    // use Miiverse, and the discovery URL has to be right before that happens rather
    // than after. Measured back when this plugin built Roseverse's token itself: a
    // console resolved Protarium's discovery host in this same process, having just been
    // handed a token, because the foreground swap hadn't run yet. Juxt has the same
    // problem for the same reason, and olv.cpp declines by itself when the selection
    // writes nothing.
    olv_apply_for_token();

    if (miiverse_routed(account, target)) {
        return Reply::Route;
    }
    return target == Miiverse::Roseverse ? Reply::Watch : Reply::Pass;
}

using RealAist = int32_t (*)(char *, const char *, uint32_t, bool, bool);

// Make the real call with the account server lent to the Miiverse's own for its length.
//
// This is the whole of both routed pairings, and it's the token request that makes it
// necessary, not the login. Protarium's account server answers Miiverse's token request
// itself and never forwards it, so Pretendo never learns that token exists, and
// Pretendo's account server answers the same request with a token Protarium never sees.
// Only the server the request reaches knows the token it gets back. An earlier version of
// this note blamed the login, that Protarium forwards logins to Pretendo unchanged, which
// describes the login path and not why a lend is needed.
//
// Juxt on Protarium. Juxt validates a token against Pretendo's records, finds nothing for
// one Protarium issued, and refuses with 115-5016, so the request goes to Pretendo's
// account server instead. Measured working on hardware.
//
// Protaverse on Pretendo. Protaverse is handed tokens Protarium's account server signs,
// so the request goes to Protarium's. It is not the same move reversed, and the
// difference is the login: a console on Protarium signed in through their proxy, and a
// console on Pretendo never touched Protarium at all. What decides it is how their
// account server answers a console it has never seen, and hardware answered that: a cold
// boot straight into Miiverse from the Wii U Menu routed the Menu's request in 2860 ms,
// the lend read lent and taken back, and Protaverse loaded. Nothing has measured
// Protaverse refusing a token Pretendo issued, the way 115-5016 measured Juxt refusing
// Protarium's, because nobody has run this pairing without the lend.
//
// Zero for the cache duration rather than whatever the caller asked for. act may be
// holding a Miiverse token the selected server issued earlier, and a caller willing to
// take a cached one would just be handed it without anything leaving the console. That
// zero meaning "fetch a new one" is documented for the 3DS's account service and not for
// this console, which is why the Debug page shows how long the call took. Hardware has
// since timed routed calls at about two and a half seconds.
int32_t route(RealAist real, char *token_out, const char *client_id, uint32_t cache, bool a,
              bool b) {
    route_cache_asked = cache;
    route_flag_a = a;
    route_flag_b = b;

    // The Miiverse's own account server, which on a routed pairing is always the one
    // that is not selected.
    const Account to = account == Account::Protarium ? Account::Pretendo : Account::Protarium;

    // One handle for the whole routed call, opened in this process and held from the
    // lend's first read until the take-back. A second handle opened for the take-back
    // would put an open that can fail, seconds later, in front of the one write here
    // that must not.
    const IosuHandle iosu;
    if (!account_lend(iosu, to)) {
        // Nothing moved, so the caller gets exactly the call it made, and the lend
        // status says why.
        ++routes_declined;
        return real(token_out, client_id, cache, a, b);
    }

    const uint64_t started = OSGetSystemTime();
    const int32_t result = real(token_out, client_id, 0, a, b);
    const uint64_t took_us = OSTicksToMicroseconds(OSGetSystemTime() - started);

    // Immediately, whatever the call returned. Until this runs, every account request on
    // the console is going to whichever server the lend just pointed at.
    account_take_back(iosu);

    const uint64_t took_ms = took_us / 1000;
    route_ms = took_ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(took_ms);
    route_result = result;
    have_route = true;
    ++requests_routed;
    if (result >= 0) {
        routed_token_fresh = true;
    }
    return result;
}

// Whether a request is Miiverse's on a routed pairing. For the applet's handler, which
// has to stay clear of everything decide() does.
bool routed_request(const char *client_id) {
    return is_miiverse(client_id) && miiverse_routed(account, target);
}

// A Wii U Menu or game request on Roseverse, made exactly the way the caller made it and
// timed. Nothing is changed on the way through: RosePatcher is the one meant to answer
// it, and this only records whether it did.
int32_t watch_roseverse(RealAist real, char *token_out, const char *client_id, uint32_t cache,
                        bool a, bool b) {
    const uint64_t started = OSGetSystemTime();
    const int32_t result = real(token_out, client_id, cache, a, b);
    const uint64_t took_ms = OSTicksToMicroseconds(OSGetSystemTime() - started) / 1000;

    roseverse_ms = took_ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(took_ms);
    roseverse_result = result;
    have_roseverse_call = true;
    ++roseverse_reached;
    return result;
}

// The applet's own request on a routed pairing, lent the way route() lends the Wii U
// Menu's and a game's, and timed.
//
// This used to be watched and never lent, on the expectation that act would hand the
// applet the token a routed call in the Wii U Menu had already fetched. That holds only
// when the Wii U Menu asks first, and it doesn't always. Launched straight after a cold
// boot, Juxt on Protarium read `Token: 2 seen, 0 routed`, `Token seen in: applet
// game/menu (pid 9)` and `Applet request: 00000000 in 286 ms, cache 43200`: the applet
// asked before anything else, nothing was cached, act fetched from Protarium's account
// server, and Juxt refused the token with an account error. Retrying right away
// worked.
//
// A forced fresh fetch goes to the server and costs about two and a half seconds, so it's
// only forced until a routed fetch has come back once. After that the applet's own cache
// duration goes through and act hands over the token it holds, which the Miiverse's own
// account server issued, and the lend still covers any fetch act decides to make.
//
// Lending from inside the applet is new, and it's IOS calls to Mocha and nothing else. The
// two freezes this plugin has caused in the applet came from a module load inside a file
// hook and from a token path that swept memory, called nn_act again and reached the SD card
// and MCP, and none of that happens here. A lend that refuses still lets the request
// through unlent, the same as route() does, and the Debug page says so.
int32_t route_applet(RealAist real, char *token_out, const char *client_id, uint32_t cache,
                     bool a, bool b) {
    const Account to = account == Account::Protarium ? Account::Pretendo : Account::Protarium;

    // Held from before the lend until after the take-back, for the reason route() gives.
    const IosuHandle iosu;
    const bool lent = account_lend(iosu, to);
    if (!lent) {
        ++routes_declined;
    }
    const uint32_t sent = lent && !routed_token_fresh ? 0 : cache;

    const uint64_t started = OSGetSystemTime();
    const int32_t result = real(token_out, client_id, sent, a, b);
    const uint64_t took_ms = OSTicksToMicroseconds(OSGetSystemTime() - started) / 1000;

    // Immediately, whatever the call returned, and a no-op when the lend refused.
    account_take_back(iosu);

    applet_ms = took_ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(took_ms);
    applet_result = result;
    applet_cache_asked = cache;
    applet_cache_sent = sent;
    applet_lent = lent;
    applet_flag_a = a;
    applet_flag_b = b;
    have_applet_call = true;

    if (lent) {
        ++requests_routed;
        if (result >= 0) {
            routed_token_fresh = true;
        }
    }
    return result;
}

} // namespace

// One wrapper per process, because a process target is a single value rather than a set
// and the generated symbols collide if one function is registered twice. They're
// identical apart from the bit they record and the real function they fall through to.
//
// The signature comes from the export name: a buffer, the client id being asked for, then
// three arguments handed on untouched, except that a routed call asks for a fresh token:
// see route().
//
// The applet, and the one handler here that isn't a copy of the others. It goes in by
// address at runtime rather than being registered statically, and it lends the account
// server around the applet's own request, the way the others do around theirs. It used to
// leave that request alone and count on act handing over what a routed call in the Wii U
// Menu had fetched, and route_applet has the reading that showed why that isn't enough.
DECL_FUNCTION(int32_t, aist_miiverse, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_MIIVERSE);
    ++requests_seen;
    if (routed_request(c)) {
        return route_applet(real_aist_miiverse, t, c, d, a, b);
    }
    return real_aist_miiverse(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_game_menu, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_GAME_MENU);
    const Reply reply = decide(c);
    if (reply == Reply::Route) return route(real_aist_game_menu, t, c, d, a, b);
    if (reply == Reply::Watch) return watch_roseverse(real_aist_game_menu, t, c, d, a, b);
    return real_aist_game_menu(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_home, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_HOME);
    const Reply reply = decide(c);
    if (reply == Reply::Route) return route(real_aist_home, t, c, d, a, b);
    if (reply == Reply::Watch) return watch_roseverse(real_aist_home, t, c, d, a, b);
    return real_aist_home(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_browser, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_BROWSER);
    const Reply reply = decide(c);
    if (reply == Reply::Route) return route(real_aist_browser, t, c, d, a, b);
    if (reply == Reply::Watch) return watch_roseverse(real_aist_browser, t, c, d, a, b);
    return real_aist_browser(t, c, d, a, b);
}

// THE APPLET'S OWN TOKEN REQUEST
//
// The Miiverse applet fetches its own sign-in token, in its own process, and the static
// replacements below never see that call.
//
// A registration naming the applet used to sit at the bottom of this file, so on paper
// the applet was covered. It is a patch by symbol name, applied when the loader sees
// nn_act loaded in a covered process. The coreinit hooks in olv.cpp prove the plugin
// backend runs in the applet, yet across every reading the token only ever showed up in
// the Wii U Menu or a game, never the applet. The applet loads nn_act after the backend
// has applied its patches, and nothing re-applies them for this process, so the by-name
// patch never lands.
//
// What reaches the applet is a patch installed inside it: resolve nn_act in the applet's
// own process, when it opens initial.oma, and patch the address that comes back rather
// than a name. olv.cpp calls the function below from that same file open.
namespace {

PatchedFunctionHandle applet_patch = 0;
bool applet_patch_installed = false;
// Read in the config menu, which is a different process from the applet. A plugin
// global outlives the process it was set in, so what the applet writes here is what
// the menu reads back.
char applet_line[64] = "not attempted";

// The mangled name every one of these patches is aimed at. The registrations at the
// bottom of this file spell it as a bare token, because that macro stringifies whatever
// you hand it. This is the same name as a string, for the runtime lookup. If one ever
// changes, change both.
constexpr const char *AIST_SYMBOL = "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4";

// The address, or zero with a reason written to `why`. Never loads the library: see the
// note in token_apply_applet_hook about what Acquire did to the console.
uint32_t resolve_act_token_function(const char **why) {
    *why = nullptr;

    OSDynLoad_Module module = nullptr;
    if (OSDynLoad_IsModuleLoaded("nn_act", &module) != OS_DYNLOAD_OK || module == nullptr) {
        *why = "nn_act not loaded here";
        return 0;
    }

    void *export_addr = nullptr;
    if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, AIST_SYMBOL, &export_addr) !=
            OS_DYNLOAD_OK ||
        export_addr == nullptr) {
        *why = "no token export";
        return 0;
    }

    return reinterpret_cast<uint32_t>(export_addr);
}

} // namespace

void token_init() {
    // libfunctionpatcher keeps the module's entry points in this plugin's own memory,
    // which lasts the whole boot, so one call in the first process is enough for the
    // applet install to work in a process I get no lifecycle hook in.
    const FunctionPatcherStatus init = FunctionPatcher_InitLibrary();
    if (init != FUNCTION_PATCHER_RESULT_SUCCESS) {
        std::snprintf(applet_line, sizeof(applet_line), "patcher init failed (%d)",
                      static_cast<int>(init));
    }
}

void token_apply_applet_hook() {
    // Called for every setting, so a patch left installed by an earlier session comes
    // back out once the setting no longer wants one. The patch outlives the applet
    // process it was added in and the applet starts fresh on every launch, so a stale one
    // is a real state to clear rather than one that can't happen.
    if (applet_patch_installed) {
        FunctionPatcher_RemoveFunctionPatch(applet_patch);
        applet_patch_installed = false;
    }

    // Only a routed pairing lends around the applet's request. On every other pairing the
    // handler would have nothing to do, and on Roseverse RosePatcher installs its own
    // answer in this same process, from the same file open, and a second patch of mine
    // on top of it is one more thing to rule out if Roseverse doesn't load.
    if (!miiverse_routed(account, target)) {
        std::snprintf(applet_line, sizeof(applet_line), "idle on this pairing");
        return;
    }

    // Ask whether nn_act is loaded. Never load it.
    //
    // That difference is why an earlier version of this froze the console.
    // OSDynLoad_Acquire loads the library when it is absent, and absent is precisely the
    // case this function exists for, so it did a module load from inside an FSOpenFile
    // replacement: the filesystem re-entered on the thread already inside it.
    // IsModuleLoaded only asks, and hands back a usable handle when the answer is yes.
    // The name is spelled without the extension, which is how the loader answers this
    // question and the same spelling olv_is_loaded uses.
    //
    // Not loaded is a real answer rather than a failure: no patch this visit, and the
    // line below says so. If that's what a console reports, the trigger is in the wrong
    // place and no amount of patching from here would have worked.
    const char *why = nullptr;
    const uint32_t effective = resolve_act_token_function(&why);
    if (effective == 0) {
        std::snprintf(applet_line, sizeof(applet_line), "%s", why);
        return;
    }

    // By address and for this process only. The effective address is what act's own
    // callers jump to; the physical one is what the patcher writes through, the same
    // pairing swap_all in scan.cpp uses for the kernel copy.
    function_replacement_data_t patch = REPLACE_FUNCTION_VIA_ADDRESS_FOR_PROCESS(
        aist_miiverse, OSEffectiveToPhysical(effective), effective, FP_TARGET_PROCESS_MIIVERSE);

    bool patched = false;
    const FunctionPatcherStatus added =
        FunctionPatcher_AddFunctionPatch(&patch, &applet_patch, &patched);
    if (added != FUNCTION_PATCHER_RESULT_SUCCESS) {
        std::snprintf(applet_line, sizeof(applet_line), "add failed (%d)",
                      static_cast<int>(added));
        return;
    }
    applet_patch_installed = true;
    // "registered" rather than "installed" would mean the add was accepted but the patch
    // hasn't taken yet, which is a state worth telling apart from a clean install if a
    // reading ever shows it.
    std::snprintf(applet_line, sizeof(applet_line), "%s in pid %u",
                  patched ? "installed" : "registered", static_cast<unsigned>(OSGetUPID()));
}

const char *token_applet_status() {
    return applet_line;
}

void token_set_target(Miiverse chosen) {
    target = chosen;
}

void token_set_account(Account chosen) {
    account = chosen;
}

size_t token_requests_seen() { return requests_seen; }
size_t token_requests_routed() { return requests_routed; }
size_t token_routes_declined() { return routes_declined; }

// The last routed call as one line: the code that came back, how long the call took, and
// the cache duration and two flags the caller passed. "none yet" before any, unless every
// attempt was declined, which the lend line then explains.
const char *token_route_status() {
    if (!have_route) {
        return routes_declined > 0 ? "none, every attempt declined" : "none yet";
    }
    std::snprintf(route_line, sizeof(route_line), "%08x in %u ms, cache %u, flags %u%u",
                  static_cast<unsigned>(route_result), static_cast<unsigned>(route_ms),
                  static_cast<unsigned>(route_cache_asked), route_flag_a ? 1u : 0u,
                  route_flag_b ? 1u : 0u);
    return route_line;
}

// The last Roseverse request that reached this plugin, in the same shape as the route
// line above, after how many did.
const char *token_roseverse_status() {
    if (!have_roseverse_call) {
        return "0 reached";
    }
    std::snprintf(roseverse_line, sizeof(roseverse_line), "%u reached, last %08x in %u ms",
                  static_cast<unsigned>(roseverse_reached),
                  static_cast<unsigned>(roseverse_result), static_cast<unsigned>(roseverse_ms));
    return roseverse_line;
}

// The applet's last request on a routed pairing, in the same shape as the route line
// above so the two read against each other. "none seen" beside an applet hook that says
// installed means the applet never asked.
const char *token_applet_request_status() {
    if (!have_applet_call) {
        return "none seen";
    }
    std::snprintf(applet_request_line, sizeof(applet_request_line),
                  "%08x in %u ms, cache %u%s, %s, flags %u%u",
                  static_cast<unsigned>(applet_result), static_cast<unsigned>(applet_ms),
                  static_cast<unsigned>(applet_cache_asked),
                  applet_cache_sent != applet_cache_asked ? " sent 0" : "",
                  applet_lent ? "lent" : "not lent", applet_flag_a ? 1u : 0u,
                  applet_flag_b ? 1u : 0u);
    return applet_request_line;
}

// Names the processes the request actually arrived in. "nowhere" with a sign-in that
// plainly asked for a token means the replacement isn't installing, not that the request
// didn't happen.
const char *token_where() {
    if (fired_mask == 0) return "nowhere yet";
    char *p = where;
    const char *const end = where + sizeof(where);
    if (fired_mask & FIRED_MIIVERSE) p += std::snprintf(p, end - p, "applet ");
    if (fired_mask & FIRED_GAME_MENU) p += std::snprintf(p, end - p, "game/menu ");
    if (fired_mask & FIRED_HOME) p += std::snprintf(p, end - p, "home ");
    if (fired_mask & FIRED_BROWSER) p += std::snprintf(p, end - p, "browser ");
    std::snprintf(p, end - p, "(pid %u)", first_upid);
    return where;
}

// Installed statically, before any setting has been read, which is why decide() checks
// the setting rather than assuming it. Same arrangement as the name hooks in
// dns.cpp and the applet hook in olv.cpp.
//
// The applet is deliberately not among these. A by-name registration for the applet
// process was here and never took effect, because the applet loads nn_act after the
// backend's patch pass and nothing re-applies it. token_apply_applet_hook above installs
// my_aist_miiverse in the applet by address instead, from the applet's own initial.oma
// open, which is the one point the applet is known to be running.

WUPS_MUST_REPLACE_FOR_PROCESS(aist_game_menu, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

WUPS_MUST_REPLACE_FOR_PROCESS(aist_home, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_HOME_MENU);

WUPS_MUST_REPLACE_FOR_PROCESS(aist_browser, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_BROWSER);
