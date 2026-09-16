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
#include "logger.h"
#include "olv.h"
#include "secret.h"

#include <wups.h>

#include <function_patcher/function_patching.h>

#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/filesystem.h>
#include <coreinit/mcp.h>
#include <coreinit/memorymap.h>
#include <coreinit/time.h>
#include <nn/act/client_cpp.h>
#include <nn/result.h>

#include <cstdio>
#include <cstring>

// The bits wut doesn't declare: the nn_act entry points this file calls, spelled as the
// real exported symbol names in an asm label.
namespace nn::act {

nn::Result GetCountry(char *outCountry) asm("GetCountry__Q2_2nn3actFPc");

// This goes straight into a %u below, so I take the whole register rather than narrowing
// it to the byte it probably is.
uint32_t GetGender(void) asm("GetGender__Q2_2nn3actFv");

nn::Result GetMiiImageUrlEx(char *outUrl, SlotNo slot) asm("GetMiiImageUrlEx__Q2_2nn3actFPcUc");

} // namespace nn::act

namespace {

// The one service I answer. Everything else passes through untouched, and that isn't
// optional: this symbol is how every online service on the console gets a token, so
// widening the match takes the whole console with it.
constexpr const char *MIIVERSE_CLIENT_ID = "87cd32617f1985439ea608c2746e4610";

// The payload the token carries.
constexpr const char *PAYLOAD_FORMAT = "%u,%s,%s,%u,%u,%u/%u/%u,%s,0";

// The marker in the Mii image url that decides one of the payload's fields.
constexpr const char *NETWORK_ACCOUNT_MARKER = "https://mii-secure.account.nintendo.net/";

// Where their plugin keeps the per-account key. I only ever read it. token.h has the
// note on why I never write one.
constexpr const char *KEY_FORMAT = "fs:/vol/external01/wiiu/olive/acc_%u_key.txt";

constexpr char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static_assert(sizeof(BASE64) == 65);

constexpr size_t PAYLOAD_MAX = 512;
constexpr size_t KEY_MAX = 256;
constexpr size_t URL_MAX = 288;
// Four output characters per three input bytes, rounded up, and a terminator.
constexpr size_t TOKEN_MAX = ((PAYLOAD_MAX + 2) / 3) * 4 + 1;

Miiverse target = Miiverse::Protaverse;

// The account server, snapshotted the same way and for the same reason as the Miiverse
// above. Routed pairings get routed rather than answered or passed on, and it takes both
// snapshots to know whether I'm looking at one.
Account account = Account::Protarium;

size_t requests_seen = 0;
size_t requests_answered = 0;
size_t requests_passed = 0;
size_t requests_routed = 0;
size_t routes_declined = 0;

// How many times the console loaded an account while I was watching. Counted rather than
// assumed, for the reason every other count here exists: a token that went stale and a
// hook that never ran look identical from the menu.
size_t account_loads = 0;

const char *status_line = "no request seen yet";

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

// Built once and reused, because a title can ask more than once and the answer doesn't
// change within a session. Keyed on the account slot: a different account is a different
// token, and handing the second account the first one's token fails in a way that looks
// like the server refusing rather than like me picking wrong.
char cached_token[TOKEN_MAX];
uint8_t cached_slot = 0;
bool have_cached = false;

void base64_encode(const uint8_t *in, size_t len, char *out) {
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
        out[o++] = BASE64[(v >> 18) & 0x3F];
        out[o++] = BASE64[(v >> 12) & 0x3F];
        out[o++] = BASE64[(v >> 6) & 0x3F];
        out[o++] = BASE64[v & 0x3F];
    }
    // Padded, because the server has no reason to accept a token that isn't.
    if (i < len) {
        const bool two = (len - i) == 2;
        const uint32_t v = (uint32_t(in[i]) << 16) | (two ? uint32_t(in[i + 1]) << 8 : 0);
        out[o++] = BASE64[(v >> 18) & 0x3F];
        out[o++] = BASE64[(v >> 12) & 0x3F];
        out[o++] = two ? BASE64[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
}

// Applies the imported constant, which is why secret.h refuses a constant of the wrong
// length.
void obfuscate(uint8_t *data, size_t len, const uint8_t *key) {
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= key[i % SECRET_LEN];
    }
}

// The console's code id and serial id as the payload wants them, bounded by the field
// widths the struct declares.
bool read_console_serial(char *out, size_t out_len) {
    const int32_t handle = MCP_Open();
    if (handle < 0) return false;

    // MCP copies this across an IPC boundary, so the buffer wants 64 byte alignment,
    // not whatever the stack happened to offer.
    alignas(64) MCPSysProdSettings settings;
    std::memset(&settings, 0, sizeof(settings));
    const MCPError err = MCP_GetSysProdSettings(handle, &settings);
    MCP_Close(handle);
    if (err != 0) return false;

    const size_t code_len = strnlen(settings.code_id, sizeof(settings.code_id));
    const size_t serial_len = strnlen(settings.serial_id, sizeof(settings.serial_id));
    if (code_len + serial_len + 1 > out_len) return false;

    std::memcpy(out, settings.code_id, code_len);
    std::memcpy(out + code_len, settings.serial_id, serial_len);
    out[code_len + serial_len] = '\0';
    return true;
}

// Read only, and a missing file is an ordinary answer rather than an error worth shouting
// about: it means their plugin has never run on this console, which is exactly what the
// status line is there to report.
bool read_account_key(uint32_t principal_id, char *out, size_t out_len) {
    char path[128];
    std::snprintf(path, sizeof(path), KEY_FORMAT, principal_id);

    std::FILE *f = std::fopen(path, "r");
    if (!f) return false;

    const bool ok = std::fgets(out, static_cast<int>(out_len), f) != nullptr;
    std::fclose(f);
    if (!ok) return false;

    // fgets keeps the newline and their key files have one. I strip the last character
    // only, not whitespace in general, so a key with a trailing space survives.
    const size_t len = std::strlen(out);
    if (len > 0 && out[len - 1] == '\n') out[len - 1] = '\0';
    return out[0] != '\0';
}

// The whole build, from an already signed-in account to a finished token. Returns false
// with status_line set to the reason, because every reason wants something different done
// about it and a bare failure looks exactly like the hook never running at all.
bool build_token(uint8_t slot) {
    const uint8_t *key = secret_bytes();
    if (!key) {
        status_line = "no constant imported";
        return false;
    }

    nn::act::Initialize();

    const uint32_t principal_id = nn::act::GetPrincipalId();
    if (principal_id == 0) {
        nn::act::Finalize();
        status_line = "no account signed in";
        return false;
    }

    char account_key[KEY_MAX] = {};
    if (!read_account_key(principal_id, account_key, sizeof(account_key))) {
        nn::act::Finalize();
        status_line = "no key file, run Rose's build once to create one";
        return false;
    }

    char country[8] = {};
    nn::act::GetCountry(country);

    const uint32_t gender = nn::act::GetGender();

    uint16_t year = 0;
    uint8_t month = 0, day = 0;
    nn::act::GetBirthday(&year, &month, &day);

    char mii_url[URL_MAX] = {};
    nn::act::GetMiiImageUrlEx(mii_url, slot);
    const uint32_t network_account = std::strstr(mii_url, NETWORK_ACCOUNT_MARKER) ? 1 : 0;

    nn::act::Finalize();

    char serial[32] = {};
    if (!read_console_serial(serial, sizeof(serial))) {
        status_line = "could not read the console serial";
        return false;
    }

    char payload[PAYLOAD_MAX] = {};
    const int written = std::snprintf(payload, sizeof(payload), PAYLOAD_FORMAT, principal_id,
                                      account_key, country, gender, network_account,
                                      static_cast<unsigned>(year), static_cast<unsigned>(month),
                                      static_cast<unsigned>(day), serial);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
        status_line = "payload did not fit";
        return false;
    }

    obfuscate(reinterpret_cast<uint8_t *>(payload), static_cast<size_t>(written), key);
    base64_encode(reinterpret_cast<const uint8_t *>(payload), static_cast<size_t>(written),
                  cached_token);

    cached_slot = slot;
    have_cached = true;
    // I never log the token, and never the payload either. Both carry the console's
    // serial and the account key, and logs are things people paste in public.
    status_line = "token built";
    LOG("miiverse token built for slot %u, %u characters", slot,
        static_cast<unsigned>(std::strlen(cached_token)));
    return true;
}

// Which moment produced the token currently cached, and in which process.
//
// A token built too early is well formed and wrong, and nothing else here can see the
// difference: the state line says "token built" either way, and the counters say answered
// either way. This is what tells a token built at an application start apart from one
// built once the account was loaded.
char build_origin[64] = "not built yet";

bool build_token_from(const char *origin, uint8_t slot) {
    if (!build_token(slot)) {
        return false;
    }
    std::snprintf(build_origin, sizeof(build_origin), "%s, pid %u", origin,
                  static_cast<unsigned>(OSGetUPID()));
    return true;
}

// The slot the console is on, with act brought up for the question and put back down
// again: at an application start act may not be up yet, and at an account load it has
// just finished changing.
uint8_t current_slot() {
    nn::act::Initialize();
    const uint8_t slot = nn::act::GetSlotNo();
    nn::act::Finalize();
    return slot;
}

// Which process the request arrived in, one bit each, plus the process id of the first
// one to show up.
//
// This exists because my first hardware run reported zero requests seen while the applet
// hook in olv.cpp reported two visits in the same session. That rules out this plugin not
// running in the applet and leaves two possibilities: the request happens in a process I
// wasn't watching, or the replacement never installed. One bit per target separates them.
// Any bit set means the first; no bit set with a request that plainly happened means the
// second.
uint32_t fired_mask = 0;
uint32_t first_upid = 0;

constexpr uint32_t FIRED_MIIVERSE = 1u << 0;
constexpr uint32_t FIRED_MINI = 1u << 1;
constexpr uint32_t FIRED_GAME_MENU = 1u << 2;
constexpr uint32_t FIRED_HOME = 1u << 3;
constexpr uint32_t FIRED_BROWSER = 1u << 4;

char where[96];

void note_process(uint32_t bit) {
    if (fired_mask == 0) {
        first_upid = static_cast<uint32_t>(OSGetUPID());
    }
    fired_mask |= bit;
}

// What answer() decided. Three outcomes rather than yes or no, because a routed pairing
// neither answers the request here nor leaves it alone.
enum class Reply {
    Pass,
    Answer,
    Route,
};

// How much a caller's token buffer is assumed to hold.
//
// The signature hands over a bare pointer with no size, so this is an assumption about a
// buffer I can't see rather than a measurement. The convention for these buffers is 512.
constexpr size_t CALLER_BUFFER_MAX = 512;

// Copy the built token into a caller's buffer without running off the end of it.
//
// This used to be a plain strcpy, which trusted the caller's buffer to be at least as
// long as whatever got built. The token comes from a payload capped at 512 bytes, so
// cached_token can hold more than CALLER_BUFFER_MAX even though a real one runs to about
// 136 characters. A long key file or a long serial is all it would take, and the result
// would be a write past the end of a buffer belonging to the Miiverse applet, which is
// the worst place on this console to be wrong.
//
// Bounded instead, copying what the token needs rather than padding to the cap: strncpy
// would zero-fill the whole 511 bytes on every call, which makes a buffer smaller than
// assumed worse rather than better. A token long enough to be truncated here gets refused
// by the server, and that's a visible failure rather than a corrupted one.
void copy_token(char *token_out) {
    size_t length = std::strlen(cached_token);
    if (length > CALLER_BUFFER_MAX - 1) {
        length = CALLER_BUFFER_MAX - 1;
    }
    std::memcpy(token_out, cached_token, length);
    token_out[length] = '\0';
}

// Answer, route or decline. Written once and called from every process wrapper below,
// which differ only in which real function they fall through to.
//
// Pass means "call the real one with everything untouched", and every path that is
// neither a Miiverse request in Roseverse mode nor one on a routed pairing passes.
// Declining isn't failing: a console with no key file or no imported constant then
// behaves exactly as it does with this plugin absent, which is the property the whole
// design rests on.
Reply answer(char *token_out, const char *client_id) {
    ++requests_seen;

    const bool is_miiverse =
        client_id != nullptr && std::strcmp(client_id, MIIVERSE_CLIENT_ID) == 0;

    // First thing, and deliberately not gated on the Roseverse setting.
    //
    // A request for this client id is my earliest warning that the process is about to
    // use Miiverse, and the discovery URL has to be right before that happens rather
    // than after. Measured: a console on Roseverse resolved Protarium's discovery host
    // in this same process, having just taken a Roseverse token from me, because the
    // foreground swap hadn't run yet. Juxt is in here because it has the same problem
    // for the same reason, and olv.cpp declines by itself when the selection writes
    // nothing.
    if (is_miiverse) {
        olv_apply_for_token();
    }

    // A routed pairing: Juxt on Protarium, or Protaverse on Pretendo. The real function
    // still makes the request, but route() below points it at the Miiverse's own account
    // server rather than the selected one.
    if (is_miiverse && miiverse_routed(account, target)) {
        return Reply::Route;
    }

    // Anything that isn't Miiverse, and every request at all when Roseverse has not been
    // chosen, gets declined. That's what keeps games on the selected account server
    // rather than on a token I made up.
    if (!is_miiverse || target != Miiverse::Roseverse) {
        ++requests_passed;
        return Reply::Pass;
    }

    const uint8_t slot = nn::act::GetSlotNo();
    if ((!have_cached || slot != cached_slot) && !build_token_from("request", slot)) {
        ++requests_passed;
        return Reply::Pass;
    }

    if (token_out != nullptr) {
        copy_token(token_out);
    }
    ++requests_answered;
    return Reply::Answer;
}

// The applet's answer, and deliberately almost nothing like answer() above.
//
// This runs inside the Miiverse applet, on the thread that's already inside act's own
// token call, so what it must not do is the whole design. My first version went through
// answer() and froze the console. Every line of that is a hazard here:
//
//   The sweep. answer() runs olv_apply_for_token() before anything else. In the applet
//   repoint_applet() has already swapped the discovery URL, during the same file open
//   that installs this patch, so there is nothing left to do and a memory sweep on
//   act's stack to do it with.
//
//   nn::act::GetSlotNo(). That's re-entering nn_act from inside an nn_act call. The slot
//   is read there only to notice an account switch between requests, which cannot happen
//   inside a single applet launch, so this copies without asking.
//
//   build_token(). Initialize and Finalize on act from inside an act call, an fopen on
//   the SD card in a process whose devoptab may never have been set up, and three MCP
//   calls. So this copies a token built earlier in the Wii U Menu or the game and stops
//   there.
//
// No cached token means passing the call through, which costs the applet a real token
// Roseverse will refuse. That's the old failure rather than a new one, and the counters
// say which happened: seen with passed on, against seen with answered.
bool answer_from_cache(char *token_out, const char *client_id) {
    ++requests_seen;

    const bool is_miiverse =
        client_id != nullptr && std::strcmp(client_id, MIIVERSE_CLIENT_ID) == 0;

    if (!is_miiverse || target != Miiverse::Roseverse || !have_cached) {
        ++requests_passed;
        return false;
    }

    if (token_out != nullptr) {
        copy_token(token_out);
    }
    ++requests_answered;
    return true;
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
// can't go through answer() and so has to ask this on its own.
bool routed_request(const char *client_id) {
    return client_id != nullptr && std::strcmp(client_id, MIIVERSE_CLIENT_ID) == 0 &&
           miiverse_routed(account, target);
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
// Five targets rather than the two I started with. That narrow pair was a guess that the
// request arrives where the swap does, and the console disagreed.
//
// The signature comes from the export name: a buffer, the client id being asked for, then
// three arguments handed on untouched, except that a routed call asks for a fresh token:
// see route().
//
// The applet, and the one handler here that isn't a copy of the other four. It goes in by
// address at runtime rather than being registered statically, and on Roseverse it answers
// from the cache or not at all. See answer_from_cache above for what it must not do and
// what happened when it did.
//
// On a routed pairing it lends the account server around the applet's own request, the
// way the other four do around theirs. It used to leave that request alone and count on
// act handing over what a routed call in the Wii U Menu had fetched, and route_applet has
// the reading that showed why that isn't enough. Roseverse still needs its answer here,
// because a token I made up is never in act's cache for the applet to find.
DECL_FUNCTION(int32_t, aist_miiverse, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_MIIVERSE);
    if (answer_from_cache(t, c)) {
        return 0;
    }
    if (routed_request(c)) {
        return route_applet(real_aist_miiverse, t, c, d, a, b);
    }
    return real_aist_miiverse(t, c, d, a, b);
}

// The in-game overlay, installed by address at runtime like the applet's and for the same
// measured reason: a registration by name doesn't reach process 7 either. It answers from
// the cache or not at all, because the overlay can no more build a token than the applet
// can. See token_apply_overlay_hook below.
DECL_FUNCTION(int32_t, aist_mini, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_MINI);
    if (answer_from_cache(t, c)) {
        return 0;
    }
    return real_aist_mini(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_game_menu, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_GAME_MENU);
    const Reply reply = answer(t, c);
    if (reply == Reply::Answer) return 0;
    if (reply == Reply::Route) return route(real_aist_game_menu, t, c, d, a, b);
    return real_aist_game_menu(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_home, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_HOME);
    const Reply reply = answer(t, c);
    if (reply == Reply::Answer) return 0;
    if (reply == Reply::Route) return route(real_aist_home, t, c, d, a, b);
    return real_aist_home(t, c, d, a, b);
}

DECL_FUNCTION(int32_t, aist_browser, char *t, const char *c, uint32_t d, bool a, bool b) {
    note_process(FIRED_BROWSER);
    const Reply reply = answer(t, c);
    if (reply == Reply::Answer) return 0;
    if (reply == Reply::Route) return route(real_aist_browser, t, c, d, a, b);
    return real_aist_browser(t, c, d, a, b);
}

// The console loading an account, which is the one moment the account data is known to be
// complete. That, and nothing else, is why the token gets rebuilt here.
//
// I got this wrong twice, and both are worth keeping so nobody tries them again.
//
// First it marked the token stale. That emptied the cache in front of the applet, the one
// process that cannot refill it, and cost the first Miiverse launch of every session: one
// request passed on, a real account-server token handed over, refused.
//
// Then it only counted. That left a token built at an application start standing even
// when that build had raced the account load and read a half-populated account. On
// hardware the result was a token that was well formed and wrong, and Roseverse answered
// it as though the console had no account set up. Neither the state line nor the counters
// could see it: both said built and answered.
//
// The placement is the whole point rather than the trigger. build_token reads account
// details that aren't true until this function has returned. An application start is earlier than that, and
// sometimes earlier than the account itself.
//
// Unconditional on purpose. There is no slot guard here: the token this exists to replace
// was built for the same slot, so a slot comparison would look straight past the thing
// it's here to fix.
//
// The real function goes first and its result is handed back untouched.
DECL_FUNCTION(int32_t, load_console_account, uint8_t slot, uint32_t option,
              const char *password, bool flag) {
    const int32_t result = real_load_console_account(slot, option, password, flag);
    ++account_loads;
    if (target == Miiverse::Roseverse) {
        build_token_from("account load", current_slot());
    }
    return result;
}

// THE APPLET'S OWN TOKEN REQUEST
//
// The Miiverse applet fetches its own sign-in token, in its own process, and the static
// replacements below never answer that call.
//
// There used to be a registration at the bottom of this file naming the applet, so on
// paper the applet was covered. It is a patch by symbol name, applied when the loader
// sees nn_act loaded in a covered process. The applet was covered and the coreinit hooks
// in olv.cpp prove the plugin backend runs here, yet across every reading the token only
// ever showed up in the Wii U Menu or a game, never the applet. The applet loads nn_act
// after the backend has applied its patches, and nothing re-applies them for this
// process, so the by-name patch never lands. The applet then asks the real account server
// for a token, and Roseverse refuses it. That's the launch that fails from the HOME menu
// overlay and works from the Wii U Menu: the Wii U Menu fetches the token in its own
// process, which is patched, and hands it to the applet, while the overlay path leaves
// the applet to fetch its own.
//
// What answers the applet is a patch installed inside it: resolve nn_act in the applet's
// own process, when it opens initial.oma, and patch the address that comes back rather
// than a name. olv.cpp calls the function below from that same file open. The handler it
// installs is my_aist_miiverse, the same one the removed registration used, so the answer
// is identical and only where it is installed changed.
namespace {

PatchedFunctionHandle applet_patch = 0;
bool applet_patch_installed = false;
// Read in the config menu, which is a different process from the applet. A plugin
// global outlives the process it was set in, so what the applet writes here is what
// the menu reads back.
char applet_line[64] = "not attempted";

} // namespace

// The mangled name every one of these patches is aimed at. The registrations at the
// bottom of this file spell it as a bare token, because that macro stringifies whatever
// you hand it. This is the same name as a string, for the runtime lookups. If one ever
// changes, change both.
constexpr const char *AIST_SYMBOL = "AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4";

namespace {

// Where nn_act's token function sits, as seen from each process that looked.
//
// This exists to answer one question nothing else here can: whether the applet has nn_act
// at the same address the Wii U Menu does. If it doesn't, a patch carrying an address
// resolved in the menu lands somewhere else entirely in the applet, which would explain
// why a registration by name never took hold there. It would also say whether the
// registration aimed at the in-game overlay ever took hold, which is the open question
// about process 7.
//
// Nothing gets patched to find this out. Both calls only read.
constexpr size_t SIGHTING_MAX = 4;

struct Sighting {
    uint32_t pid;
    uint32_t address;
    // The instruction sitting at the entry when this looked, which is what says
    // whether anything had already patched the function in that process.
    uint32_t first_word;
};

Sighting sightings[SIGHTING_MAX];
size_t sighting_count = 0;
char act_line[96];

// The address, or zero with a reason written to `why`. Never loads the library: see
// the note in token_apply_applet_hook about what Acquire did to the console.
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

// One sighting per process, kept for the whole boot. A second look from the same
// process would answer the same thing and would push a different process out of the
// list, which is the one thing this must not do.
void note_act_address() {
    const uint32_t pid = static_cast<uint32_t>(OSGetUPID());
    for (size_t i = 0; i < sighting_count; ++i) {
        if (sightings[i].pid == pid) {
            return;
        }
    }
    if (sighting_count >= SIGHTING_MAX) {
        return;
    }

    const char *why = nullptr;
    const uint32_t address = resolve_act_token_function(&why);
    sightings[sighting_count].pid = pid;
    sightings[sighting_count].address = address;
    // Read once, here, rather than when the menu asks. In the applet this runs before
    // the patch below is installed, so what it records is the state I found rather than
    // the state I made, which is the only version worth having.
    sightings[sighting_count].first_word =
        address != 0 ? *reinterpret_cast<const uint32_t *>(address) : 0;
    ++sighting_count;
}

// A patched function doesn't start the way a function starts.
//
// Two shapes say so. An unconditional branch, primary opcode 18, is what the patcher
// writes when its replacement is within reach. A load of r12 followed by a jump through
// the count register is what it writes when it isn't, and that starts with addis to r12,
// primary opcode 15. Anything else reads as unpatched.
//
// Past tense on purpose. This is read once, when the process is first seen, and in the
// applet and the overlay that's before I install my own patch there. So it reports what
// was found on arrival rather than what is true now, and a console showing "found
// unpatched" beside a hook that says installed is the two of them agreeing rather than
// contradicting. An earlier wording said "untouched" in the present tense and read as a
// live claim, which is exactly the wrong thing for a line whose whole value is saying
// what was there beforehand.
//
// The raw word is printed beside this so you can check the verdict rather than believe
// it. A wrong guess here should be visible, not silent.
const char *patch_verdict(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 18) {
        return "found patched";
    }
    if (opcode == 15 && ((word >> 21) & 31) == 12) {
        return "found patched";
    }
    return "found unpatched";
}

// A foothold in the in-game Miiverse overlay, process 7, which nothing in this plugin has
// ever run in.
//
// It observes and does nothing else. No patch goes in here, so it can't take away
// whatever coverage the registration for that process may already be providing, and can't
// stack a second patch on a trampoline that already has one. Both of those were the
// reasons not to touch process 7 without knowing its current behavior, and neither
// applies to reading an address.
//
// coreinit for the replacement, because coreinit is loaded in every process from the
// start, which is why the applet's file hooks work where its nn_act registration didn't.
// Any file the overlay opens will do: I want a moment when the process is running, not a
// particular file.
bool overlay_looked = false;

PatchedFunctionHandle overlay_patch = 0;
bool overlay_patch_installed = false;
char overlay_line[64] = "not attempted";

} // namespace

// The same install the applet gets, in the overlay's own process.
//
// Measured before this existed: `nn_act pid 7: 0f5e7d88 9421ff18 untouched`, where that
// word is an ordinary stwu prologue. So the registration by name aimed at this process
// never took hold, exactly as it never took hold in the applet.
//
// What I inferred from that, and shouldn't have, is that the overlay was therefore asking
// the real account server and being refused. It is not asking. A post from inside a game
// on Roseverse succeeded with this installed and with no `overlay` in `Token seen in`,
// meaning the handler never ran: the game fetches the token, which is the `game/menu` in
// that same line, and the overlay uses what the game holds.
//
// I keep it because it costs a bool test per file open once installed and would cover a
// title whose overlay does ask. One game has been tried, so whether that generalizes is
// unmeasured. It is dormant on every reading so far.
//
// aist_mini is the handler, reused rather than replaced, and its registration by name is
// gone from the bottom of this file. Two patches on one function sharing a single real_
// pointer would corrupt the chain between them, which is why the applet's registration
// went when its runtime patch arrived, and why this one goes the same way. The handle is
// its own for the same reason: removing one must not disturb the other.
//
// Retried rather than attempted once. The applet has initial.oma, a file it's known to
// open while starting. This has no such landmark and takes any file the overlay opens, so
// a first open that lands before nn_act is loaded has to be allowed to fail and be tried
// again.
void token_apply_overlay_hook() {
    if (overlay_patch_installed) {
        return;
    }

    if (target != Miiverse::Roseverse) {
        std::snprintf(overlay_line, sizeof(overlay_line), "idle, not Roseverse");
        return;
    }

    const char *why = nullptr;
    const uint32_t effective = resolve_act_token_function(&why);
    if (effective == 0) {
        std::snprintf(overlay_line, sizeof(overlay_line), "%s", why);
        return;
    }

    function_replacement_data_t patch = REPLACE_FUNCTION_VIA_ADDRESS_FOR_PROCESS(
        aist_mini, OSEffectiveToPhysical(effective), effective,
        FP_TARGET_PROCESS_MINI_MIIVERSE);

    bool patched = false;
    const FunctionPatcherStatus added =
        FunctionPatcher_AddFunctionPatch(&patch, &overlay_patch, &patched);
    if (added != FUNCTION_PATCHER_RESULT_SUCCESS) {
        std::snprintf(overlay_line, sizeof(overlay_line), "add failed (%d)",
                      static_cast<int>(added));
        return;
    }

    overlay_patch_installed = true;
    std::snprintf(overlay_line, sizeof(overlay_line), "%s in pid %u",
                  patched ? "installed" : "registered", static_cast<unsigned>(OSGetUPID()));
}

// Dropped at every title start, so the next overlay session installs into its own
// process rather than trusting a patch added in a previous one. The overlay is opened
// from inside a game, so a game's application start always comes first.
void token_reset_overlay_hook() {
    if (!overlay_patch_installed) {
        return;
    }
    FunctionPatcher_RemoveFunctionPatch(overlay_patch);
    overlay_patch_installed = false;
    overlay_patch = 0;
}

const char *token_overlay_status() {
    return overlay_line;
}

DECL_FUNCTION(int, fs_open_overlay, FSClient *client, FSCmdBlock *block, char *path,
              const char *mode, uint32_t *handle, int error) {
    // After the real open, so nothing here can stand between the overlay and a file it
    // asked for. The address is read once; the install is attempted until it takes,
    // and costs one bool test on every file open afterwards.
    const int result = real_fs_open_overlay(client, block, path, mode, handle, error);
    if (!overlay_looked) {
        overlay_looked = true;
        note_act_address();
    }
    token_apply_overlay_hook();
    return result;
}

void token_note_act_address() {
    note_act_address();
}

size_t token_act_sighting_count() {
    return sighting_count;
}

// One process per call, because four of these on one line ran off the edge of the
// menu and cost a console run. The lookup list above it has always been a row each.
const char *token_act_sighting(size_t index) {
    if (index >= sighting_count) {
        return "";
    }

    const Sighting &seen = sightings[index];
    if (seen.address == 0) {
        std::snprintf(act_line, sizeof(act_line), "pid %u: nn_act not loaded",
                      static_cast<unsigned>(seen.pid));
    } else {
        std::snprintf(act_line, sizeof(act_line), "pid %u: %08x %08x %s",
                      static_cast<unsigned>(seen.pid), static_cast<unsigned>(seen.address),
                      static_cast<unsigned>(seen.first_word), patch_verdict(seen.first_word));
    }
    return act_line;
}

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

// Build the token before anything can ask for it, once per title start.
//
// The applet can't build one. answer_from_cache copies or passes through, and that is
// deliberate: building reads the key file off the SD card and asks MCP for the console
// serial, in a process whose devoptab may never have been set up. So the cache has to be
// warm before the applet asks, and only the Wii U Menu or a game can warm it.
//
// Measured, after a cold boot straight into Miiverse from the Home Screen: the applet
// asked first, `Token seen in` named process 9 as the first to arrive, the cache was
// empty, the request was passed through, and Roseverse refused the real token that
// came back. One request passed on, one answered later when the menu asked. Going
// back to the menu and launching again worked, because by then the menu had built it.
//
// I left this out at first, on the grounds that building at application start puts the
// filesystem somewhere logger.h says not to put it. That concern was real and is now
// outweighed: build_token already runs in these same processes on every token request,
// and this is gated to Roseverse and to a cache that is missing or built for another
// account, so it runs at most once per boot.
//
// Initialize and Finalize around the slot read because act may not be up yet this early
// in a title start, which is not true of a token request.
void token_warm_cache() {
    if (target != Miiverse::Roseverse) {
        return;
    }

    const uint8_t slot = current_slot();
    if (have_cached && slot == cached_slot) {
        return;
    }

    // This guarantees the cache isn't empty when the applet asks. It does not guarantee
    // the token is right: an application start can land before the account has finished
    // loading, and the account-load hook above is what corrects that.
    build_token_from("app start", slot);
}

void token_apply_applet_hook() {
    // Recorded first and on every setting, because where nn_act sits in this process
    // is worth knowing whether or not anything ends up patched here.
    note_act_address();

    // Called for every setting, so a patch left installed by an earlier session comes
    // back out once the setting no longer wants one. The patch outlives the applet
    // process it was added in and the applet starts fresh on every launch, so a stale one
    // is a real state to clear rather than one that can't happen.
    if (applet_patch_installed) {
        FunctionPatcher_RemoveFunctionPatch(applet_patch);
        applet_patch_installed = false;
    }

    // Roseverse answers the applet's request and a routed pairing watches it. On every
    // other pairing the handler would have nothing to do.
    if (target != Miiverse::Roseverse && !miiverse_routed(account, target)) {
        std::snprintf(applet_line, sizeof(applet_line), "idle on this pairing");
        return;
    }

    // Ask whether nn_act is loaded. Never load it.
    //
    // That difference is the whole reason my first version of this froze the console.
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
    // A changed selection invalidates a built token, because the next request may
    // have to go to the real function instead of being answered here.
    have_cached = false;
}

void token_set_account(Account chosen) {
    account = chosen;
}

size_t token_requests_seen() { return requests_seen; }
size_t token_requests_answered() { return requests_answered; }
size_t token_requests_passed() { return requests_passed; }
size_t token_requests_routed() { return requests_routed; }
size_t token_routes_declined() { return routes_declined; }
size_t token_account_loads() { return account_loads; }
const char *token_build_origin() { return build_origin; }
const char *token_status() { return status_line; }

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

// Names the processes the request actually arrived in, which is the reading this build
// exists to take. "nowhere" with a sign-in that plainly asked for a token means the
// replacement isn't installing, not that the request didn't happen.
const char *token_where() {
    if (fired_mask == 0) return "nowhere yet";
    char *p = where;
    const char *const end = where + sizeof(where);
    if (fired_mask & FIRED_MIIVERSE) p += std::snprintf(p, end - p, "applet ");
    if (fired_mask & FIRED_MINI) p += std::snprintf(p, end - p, "overlay ");
    if (fired_mask & FIRED_GAME_MENU) p += std::snprintf(p, end - p, "game/menu ");
    if (fired_mask & FIRED_HOME) p += std::snprintf(p, end - p, "home ");
    if (fired_mask & FIRED_BROWSER) p += std::snprintf(p, end - p, "browser ");
    std::snprintf(p, end - p, "(pid %u)", first_upid);
    return where;
}

// Installed statically, before any setting has been read, which is why answer() checks
// the setting rather than assuming it. Same arrangement as the name hooks in dns.cpp and
// the applet hook in olv.cpp.
//
// The applet is deliberately not among these. A by-name registration for the applet
// process was here and never took effect, because the applet loads nn_act after the
// backend's patch pass and nothing re-applies it. token_apply_applet_hook above installs
// my_aist_miiverse in the applet by address instead, from the applet's own initial.oma
// open, which is the one point the applet is known to be running.
//
// The in-game overlay is deliberately not among these either, for the reason the applet
// is not: measured `untouched` in process 7, so the registration never took hold.
// token_apply_overlay_hook installs aist_mini there by address instead, from the
// overlay's own file opens.

WUPS_MUST_REPLACE_FOR_PROCESS(aist_game_menu, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

WUPS_MUST_REPLACE_FOR_PROCESS(aist_home, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_HOME_MENU);

WUPS_MUST_REPLACE_FOR_PROCESS(aist_browser, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCcUibT4,
                              WUPS_FP_TARGET_PROCESS_BROWSER);

// Game and menu, which is where accounts are loaded. The applet doesn't load console
// accounts, and a registration by name wouldn't reach it anyway.
WUPS_MUST_REPLACE_FOR_PROCESS(load_console_account, WUPS_LOADER_LIBRARY_NN_ACT,
                              LoadConsoleAccount__Q2_2nn3actFUc13ACTLoadOptionPCcb,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);

// The in-game Miiverse overlay, and the only thing this plugin has ever run in that
// process. It reads an address and patches nothing. coreinit rather than nn_act on
// purpose: coreinit is loaded in every process from the start, which is why the applet's
// file hooks land where its nn_act registration did not, and this needs a replacement
// that's certain to run rather than one that is the thing in question.
WUPS_MUST_REPLACE_FOR_PROCESS(fs_open_overlay, WUPS_LOADER_LIBRARY_COREINIT, FSOpenFile,
                              WUPS_FP_TARGET_PROCESS_MINI_MIIVERSE);
