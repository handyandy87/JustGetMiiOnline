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

#include <pthread.h>

#include <atomic>
#include <cstddef>

// One look per boot at this plugin's releases on GitHub, and a toast when the release
// line this console runs has a newer tag than the one installed, as in "JustGetMiiOnline
// v1.0.4 is available". A v1 install only hears about v1.x and a v2 install only about
// v2.x, because the two lines want different companions and moving between them is a
// decision, not an update.
//
// The answer comes from GET /repos/handyandy87/JustGetMiiOnline/releases, made with the
// libcurl in Aroma's CURLWrapperModule and checked against the root bundle that module
// carries. /releases/latest can't be used: every v2 release is a pre-release, and latest
// skips those.
//
// The console's own NSSL was the first transport, and hardware turned it down: the
// blocking handshake failed with -0x28000a, NSSL's library error. api.github.com takes
// only AES-GCM suites on TLS 1.2, and NSSL has none to offer. The module's mbedTLS does.
//
// Anything that goes wrong shows nothing to the user. The Debug page says what happened,
// and tells the module, the lookup, the connection, TLS and HTTP apart.
//
// WHEN, AND WHY NOT EARLIER.
//
// A socket at plugin initialization hung this console once, and so did the SD card at
// an application start during boot, which logger.h has. So nothing here runs at either.
// update_start acts only in the Wii U Menu and hands the work to a thread that waits
// before it touches the network, so the menu is drawn and its own startup is over first. It's
// called from ON_ACQUIRED_FOREGROUND, and from ON_APPLICATION_START with a longer wait,
// because at power-on the menu starts with the foreground and never acquires it. The thread fills
// static fields and nothing else. It never posts the toast: whether NotificationModule
// takes a call from a thread of mine is something I couldn't check, so update_post does
// that from the hooks, on the thread the system runs them on.
//
// STOPPED AND JOINED AT APPLICATION END.
//
// The thread belongs to the Wii U Menu's process, and it doesn't outlive it. A relaunch
// from the menu shortly after boot froze the console while the thread was still waiting
// out its start or inside curl, so ON_APPLICATION_ENDS now asks it to stop and waits for
// it, the way AromaBasePlugin stops its own update thread and wiiload_plugin its receiver
// from the same hook. Its waits are 100 ms steps that look at the stop flag, and curl
// looks at it too, from its progress callback, so a stop lands within about a second
// wherever the thread is. update_get has the one place that can take longer.

// Whether the next boot checks. Read from storage in Config::Init, and on unless the
// user turned it off. Changing it doesn't need a reboot and doesn't stop a check that's
// already running; it decides whether the next one starts.
void update_set_enabled(bool enabled);
bool update_enabled();

// Starts the check, once per boot. Call from ON_ACQUIRED_FOREGROUND, and from
// ON_APPLICATION_START with at_boot set for the longer wait. Does nothing outside
// the Wii U Menu, with the check off, or once a check has run to an answer. The flag that
// says so lives in this plugin's memory, which every process shares, so a return to the
// menu after a game doesn't count as a new boot.
void update_start(bool at_boot = false);

// Queues the toast if the check found a newer release and it hasn't been queued yet.
// Call from the hooks that come after the check: foreground changes and application
// starts. Cheap when there's nothing to do.
void update_post();

// Call from ON_APPLICATION_ENDS. A menu that closes while the check is still running
// marks it interrupted, stops the thread and joins it before the process goes, and the
// next time the menu starts or has the foreground the check starts again. That's still
// one answer per boot.
void update_app_ends();

// The Debug page's "Update check: ..." line, without the label. Formatted on each call
// from what the thread left, so the page never reads a string while it's being written.
void update_status(char *out, size_t size);

// WHAT THE INSTALLER SHARES WITH THE CHECK.
//
// update_install.cpp downloads the release this found, through the same module and the
// same request code, so these are here for it and nothing else needs them.

// The newer release, as the check read it out of the same answer. Both links are built
// from this repo and the tag; has_wps and has_checksum say the release actually lists
// them. notes_whole says the release notes arrived uncut, which is the only way `manual`,
// the marker update.cpp names, can be trusted to be absent.
struct UpdateRelease {
    int major = -1;
    int minor = 0;
    int patch = 0;
    char wps_url[128] = {};
    char checksum_url[136] = {};
    bool has_wps = false;
    bool has_checksum = false;
    bool notes_whole = false;
    bool manual = false;
};

// True, with *out filled, only once the check has answered that a newer release exists.
bool update_newer(UpdateRelease *out);

// Why a request got no answer, in the same terms the "Update check" line uses. `code` is
// the CURLcode, or the HTTP status for HTTP_STATUS, `tls_code` is mbedTLS's own error
// from the message curl leaves, and `verify` is mbedTLS's certificate flags when the
// chain didn't check out.
enum class UpdateFault {
    NONE,
    NO_MODULE,
    MODULE_INCOMPLETE,
    CURL_INIT,
    NO_LOOKUP,
    NO_CONNECT,
    TLS_FAILED,
    TIMED_OUT,
    CURL_FAILED,
    HTTP_STATUS,
};

struct UpdateFailure {
    UpdateFault fault = UpdateFault::NONE;
    int code = 0;
    int tls_code = 0;
    long verify = 0;
    const char *missing = "";
    // The start of curl's own message, for a TLS failure that carries no mbedTLS code.
    char detail[48] = {};
};

void update_failure_text(const UpdateFailure &failure, char *out, size_t size);

// curl's write callback: answering less than size * count ends the transfer.
using UpdateSink = size_t (*)(char *data, size_t size, size_t count, void *user);

// One GET of `url` on the calling thread, through CURLWrapperModule, verified against its
// roots, https only, and following redirects, which GitHub's release links always take.
// At most max_bytes when the server states a length; the sink has to count for itself
// when it doesn't. True when the whole body arrived with a 200. Never call it while the
// check is running: they share curl's error buffer.
//
// `stop` set ends the transfer from curl's progress callback, which curl calls at least
// once a second while it connects, shakes hands and reads. The exception is the name
// lookup, if the module's curl resolves without a thread of its own: then the lookup
// blocks inside the console's resolver until that gives up, and the stop waits for it.
bool update_get(const char *url, UpdateSink sink, void *user, long max_bytes,
                const std::atomic<bool> *stop, UpdateFailure *out);

// A thread of the check's or the installer's, joinable, with the stack curl's mbedTLS
// needs, and the flag that asks it to stop. One per job, kept in static memory.
struct UpdateThread {
    pthread_t id{};
    bool live = false;
    std::atomic<bool> stop{false};
    // The thread's own OSThread, so update_stop can tell when it's called from it.
    std::atomic<void *> self{nullptr};
    void *(*body)(void *) = nullptr;
    void *arg = nullptr;
};

// Starts `body` on `thread`, first joining a run of it that has ended but wasn't joined
// yet. False when it couldn't be made.
bool update_spawn(UpdateThread *thread, void *(*body)(void *), void *arg);

// Sets the stop flag and joins the thread. The join has no deadline of its own: what
// bounds it is that every wait checks the flag, which update_get sets out. Giving up on
// the thread instead would leave it running while the process goes, which is the hang
// this is here to prevent. Called from the thread itself it only detaches, since a thread
// can't wait for itself.
void update_stop(UpdateThread *thread);

// Sleeps `ms` in 100 ms steps. False as soon as the thread is asked to stop.
bool update_wait(const UpdateThread *thread, int ms);
