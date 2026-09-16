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

// Three settings, on top of Protarium's Inkay, which stays installed and in charge
// of everything else: which account server signs the console's tokens, which
// Miiverse it talks to, and which network serves SpotPass.
//
// The Miiverse swap happens in the gap the loader leaves between their module's write
// and the title's own entry point, keyed on what their module leaves behind, so their
// release cadence costs this plugin nothing. olv.h sets that out.
//
// The account server is the biggest of the three, because three things follow
// from it rather than one. It writes the account address, it turns off the game
// redirects their build performs by name, and it moves SpotPass to the same
// network wherever the pairing offers only one SpotPass. Those three were separate
// questions until it became clear they have one answer: a game server, a tasksheet
// and a sign-in token all belong to whichever account server issued the token, so
// a console with one of them on the other network isn't in a configuration
// anybody intended. That's why the redirects aren't a setting of their own, and why
// SpotPass became one only where Roseverse gives it a second answer, set out below.
//
// The account server comes first and the Miiverse starts from it, because each account
// server signs into one Miiverse by itself and hardware settled which: Juxt with
// Protarium accounts fails with 115-5016, and the same console with only the account
// server moved to Pretendo works. The pairing is a default, not a lock. Juxt stays
// selectable on Protarium and Protaverse on Pretendo, and on each of those the token
// request alone goes to the Miiverse's own account server. choice.h has the rule, by
// miiverse_routed. A later switch of account server leaves the Miiverse where it is,
// which account_changed in config.cpp sets out.
//
// Roseverse is the third Miiverse and the one that doesn't follow, because it's the
// one that isn't paired. It never asks an account server for a sign-in token: Project
// Rose's RosePatcher plugin builds one on the console instead, from the principal ID and
// a per-account key it keeps on the SD card. So there is no issuer to be wrong and no
// measured column, and the setting is legal under both. What the account server still
// decides is which principal ID that is, and Protarium's account server is a proxy in
// front of Pretendo's rather than one of its own, so the answer is the same number
// either way.
//
// SpotPass used to follow those two rather than being asked about, and it's the
// third setting now. What changed is that Roseverse has a SpotPass of its own, so the
// two Roseverse pairings have two answers and each one gives the other up:
// Protarium's carries Splatfests, Conquest and 100 Mario, and Rose's carries the
// WaraWara Plaza of the Miiverse actually in use. A console gets one of them. On
// Protarium the second answer stays off the menu unless Advanced Overrides, on the
// Debug page, is on, because a console on Protarium should keep Protarium's.
//
// It's still not a free choice. choice.h holds which values a pairing allows, the
// menu offers nothing else, and a stored value that isn't allowed falls back to that
// pairing's default. Protarium's stays the default wherever Protarium is the account
// server, because choosing a Miiverse isn't choosing to give up Splatfests.
//
// The defaults cost nothing, because on them this plugin writes nothing. Every body
// here starts by checking the setting, so an absent, unreadable or unrecognized
// stored value all end up in the same place: doing nothing.
//
// Memory search and replace is how two patchers end up destroying each other, so
// the rule in this project is: don't. This is the one place that breaks the rule,
// and the only place allowed to. There's no way to move Miiverse without it: the
// address isn't fixed and nobody exports it. So this is an exception, and it's
// narrowed by two promises rather than by luck. It never searches for Nintendo's
// string, which is the key their module consumes, only for what their module leaves
// behind, which nothing else wants. And every write is exactly as long as what it
// matched. Both are checkable by reading olv.cpp, and both have to survive any later
// change to it.
//
// The first of those promises was broken once, deliberately, and put back. Taking
// Nintendo's string got ahead of the library reading the URL, and it cost their
// module a full failed scan of memory at every title start, which hardware
// measured as five second transitions becoming forty. What gets ahead without
// taking anything is the gap the loader leaves before the title's entry point, and
// that's where the swap happens now, with no module needed. olv.h has the detail.

// The sign-in token is the one part of Roseverse this plugin doesn't supply. Everything
// else Roseverse needs from the console, the discovery URL, the applet's allowlist and
// suffix list, and Rose's SpotPass, this plugin writes, the same as for the other two
// Miiverses. The token comes from RosePatcher, installed alongside this plugin, which
// answers Miiverse's request itself and keeps its own account key. token.h has how the
// two plugins' hooks sit together and what RosePatcher's own options do to them.
//
// Whether Juxt accepts the sign-in token was the question the account server
// setting was added to answer, and hardware answered it: Protarium's account
// server answers that request itself rather than passing it upstream, and Juxt
// checks tokens against Pretendo's own records, so the swap lands and Miiverse
// still refuses with 115-5016. That's why the account server is a setting, and
// why the menu counts what was swapped: without the counts, a refusal and a swap
// that never landed would look the same.
//
// Juxt on Protarium gets around that by moving only the one request. When Miiverse
// asks for its token with Juxt selected on Protarium, token.cpp points the account
// server at Pretendo for the length of that call and account.cpp puts Protarium's
// address back afterwards, so the token comes from the server Juxt checks against.
// Protaverse on Pretendo lends the account server the other way, to Protarium, whose
// account server signs the tokens Protaverse is handed. It isn't a mirror of Juxt on
// Protarium, because a console on Pretendo never signed in through Protarium, and
// route() in token.cpp says why their account server should answer it anyway.

#include "account.h"
#include "config.h"
#include "dns.h"
#include "logger.h"
#include "notify.h"
#include "olv.h"
#include "rosepatcher.h"
#include "spotpass.h"
#include "token.h"

#include <wups.h>

WUPS_PLUGIN_NAME("JustGetMiiOnline");
WUPS_PLUGIN_DESCRIPTION("Account server, Miiverse and SpotPass selection for Protarium");
WUPS_PLUGIN_VERSION("v2.0.1");
WUPS_PLUGIN_AUTHOR("HandyAndy87");
WUPS_PLUGIN_LICENSE("GPLv3");

WUPS_USE_STORAGE("justgetmiionline");

// Without this there's no filesystem device registered for this plugin at all, only the
// socket one, and every fopen fails before it has even looked at the path. It's what
// rosepatcher.cpp needs to read and write RosePatcher's config file.
//
// It showed up after an earlier hardware attempt, reading a different file, reported a
// missing file that was sitting on the card, which is the failure this produces: the open
// fails identically whether the path is wrong, the file is absent, or no device exists to
// ask.
WUPS_USE_WUT_DEVOPTAB();

INITIALIZE_PLUGIN() {
    log_open();

    Config::Init();
    account_init();

    // Set up the runtime function patcher, which token.cpp uses to install the token
    // lend inside the Miiverse applet on a routed pairing. Once per boot in this first
    // process is enough: the resolved entry points live in this plugin's memory, which
    // the applet shares. The applet is a process this plugin gets no lifecycle hook in.
    token_init();

    // Last, and after Config::Init rather than before it, so the toast names the
    // settings this boot will actually use rather than whatever storage held before an
    // unrecognized value fell back.
    notify_boot(Config::account, Config::miiverse);
}

DEINITIALIZE_PLUGIN() {
    notify_shutdown();
    account_shutdown();
    log_close();
}

ON_APPLICATION_START() {
    // A new title is a new process. The sweep has to run again in it, and the
    // callback list it owns is empty until this registers with it. Neither is
    // carried over, even though the flags that track them are.
    reset_olv_sweep();

    olv_watch_late_loads();

    // Arms the trigger that runs after their module's write and before this title
    // does anything. Arming rather than acting, because the moment that matters is
    // later than this one: every plugin's application-start hook runs inside the
    // plugin backend's, and the backend is always last in the module order, so this
    // runs before their Miiverse write instead of after it.
    olv_arm_premain();

    // A new title also gets its own lookup counters, so the menu describes the
    // game that's running rather than the first one after a reboot.
    reset_dns_counters();

    // Before the write, and deliberately not at plugin initialization. Their
    // module is only worth asking once their plugin has initialized it, and two
    // plugins initializing have no order between them. Every plugin has had its
    // turn by an application start, so this is the earliest moment the answer
    // means anything. It writes nothing and reads no setting.
    account_observe();

    // Their module writes the account URL when it initializes at boot, so an
    // application start is comfortably later. Confirmed on hardware, by rewriting
    // the address after a module had already written its own: Wii U Chat connected
    // on one account server and errored on the other, which it couldn't do if the
    // console cached the boot value.
    account_apply(Config::account);

    // SpotPass at the same point in the lifecycle and for the same reason: their
    // module writes these at boot and the console re-reads rather than caches them.
    // Which network it goes to is now the user's answer rather than one derived
    // here, and Config::Init has already forced it back to this pairing's default
    // if what was stored isn't something this pairing allows.
    spotpass_apply(Config::spotpass);
}

ON_ACQUIRED_FOREGROUND() {
    // Not application start. Their module does its own Miiverse work in the hook
    // that runs after every plugin's application-start hook, so acting at
    // application start would mean writing first and being overwritten. Acquiring
    // the foreground is later than both, which is the whole reason this is the
    // trigger.
    olv_apply_foreground();
}

ON_APPLICATION_ENDS() {
    // RosePatcher's two options, set here and nowhere earlier, because this is the first
    // moment after the config menu has closed and nothing can write RosePatcher's own copy
    // back over them before the relaunch reloads it. rosepatcher.h has why. Does nothing
    // unless the menu asked for it just before relaunching.
    rosepatcher_sync(Config::miiverse);
}
