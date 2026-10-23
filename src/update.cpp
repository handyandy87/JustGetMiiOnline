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

#include "update.h"
#include "logger.h"
#include "notify.h"

#include <coreinit/debug.h>
#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// What WUPS_PLUGIN_VERSION in main.cpp expands to: "version=" followed by that line's
// literal, in the plugin's .wups.meta section, which the loader copies into memory with
// the rest of the data. Reading it here means the version Aroma lists and the version
// this compares are one string, and the release workflow's check of that line covers
// both. If WUPS ever renames the symbol, the link fails rather than the check drifting.
extern "C" const char wups_meta_version[];

namespace {

constexpr const char URL[] =
    "https://api.github.com/repos/handyandy87/JustGetMiiOnline/releases?per_page=10";

// Where a release's files are, by tag, and the one the installer takes. The workflow
// uploads both under these names.
constexpr const char DOWNLOAD_BASE[] =
    "https://github.com/handyandy87/JustGetMiiOnline/releases/download/";
constexpr const char ASSET_NAME[] = "JustGetMiiOnline.wps";

// A release whose notes carry this line is one the menu won't install, only point at:
// one that needs a companion updated alongside it, say, or a move nobody should make
// without reading the notes first. An HTML comment, so it doesn't show on the Releases
// page. The second spelling is the same line with its angle brackets escaped, in case
// GitHub's JSON ever starts escaping them. build.yml says how to add it.
constexpr const char MANUAL_MARKER[] = "<!-- jgmo: manual -->";
constexpr const char MANUAL_MARKER_ESCAPED[] = "\\u003c!-- jgmo: manual --\\u003e";

// The Wii U Menu. plaza.cpp keys on the same number.
constexpr uint32_t WII_U_MENU_UPID = 2;

// Long enough for the menu to finish drawing and for its own network startup to settle,
// short enough that the answer is in before most people have picked anything.
constexpr int START_DELAY_SECONDS = 5;

// The wait when the start comes from the menu's application start instead. At power-on the
// menu already holds the foreground, so it never acquires it and the foreground start alone
// never came: hardware read "not run yet" until HOME was opened and closed. Application start
// is still boot, so this waits long enough for the menu and the network to be up, the way
// AromaBasePlugin's own check does from the same hook.
constexpr int BOOT_DELAY_SECONDS = 15;

// Set by update_start before the thread is made, read once by the thread.
int delay_seconds = START_DELAY_SECONDS;

// The check's thread. update_app_ends stops and joins it.
UpdateThread checker;

// How often a wait looks at the stop flag.
constexpr int WAIT_STEP_MS = 100;

// A console that has only just come up may not have an address yet. A few tries a few
// seconds apart cover that, and a console with no network at all costs a quiet thread.
constexpr int LOOKUP_TRIES = 4;
constexpr int LOOKUP_GAP_SECONDS = 5;

constexpr long CONNECT_SECONDS = 5;
// A read that moves nothing for this long ends the transfer, which is curl's spelling of
// a per-read deadline. The whole request gets the second number, as a backstop.
constexpr long STALL_SECONDS = 10;
constexpr long TOTAL_SECONDS = 30;
// A download's backstop. The stall limit above is what ends a dead one; this only stops
// a crawl that never quite stalls from holding the thread forever.
constexpr long DOWNLOAD_SECONDS = 180;

// The first ten releases come to more than this once every asset and author is counted,
// but they arrive newest first, so what gets cut off is the oldest. Static rather than
// on the thread's stack, and nothing else here allocates.
constexpr int ANSWER_MAX = 24 * 1024;
char answer[ANSWER_MAX + 1];
int answer_used = 0;

// curl and mbedTLS run their handshake on this thread's stack, inside the module, and
// mbedTLS's big-number arithmetic is the deep part. 128 KB is what a thread gets by
// default, which is what AromaBasePlugin's update thread runs its curl on.
constexpr size_t THREAD_STACK = 128 * 1024;

// CURLWrapperModule, which ships with Aroma, carries libcurl 8.7.1 on mbedTLS 2.28 and
// exports it by name. That TLS is what reaches GitHub at all: api.github.com takes only
// AES-GCM suites on TLS 1.2, and the console's own NSSL refused it with -0x28000a.
//
// It's reached through OSDynLoad rather than through libcurlwrapper, which does the same
// thing for every curl function, because this needs seven of them and a library
// would also need curl's headers. The numbers below are curl's own and are fixed by its
// ABI: an option is its type's base plus an index, both in curl.h at curl-8_7_1. A
// console without the module has nothing to acquire, and the check says so.
constexpr const char CURL_MODULE[] = "homebrew_curlwrapper";

constexpr int CURLOPT_FOLLOWLOCATION = 52;
constexpr int CURLOPT_MAXREDIRS = 68;
constexpr int CURLOPT_MAXFILESIZE = 114;
constexpr int CURLOPT_PROTOCOLS_STR = 10318;
constexpr int CURLOPT_REDIR_PROTOCOLS_STR = 10319;
constexpr int CURLOPT_WRITEDATA = 10001;
constexpr int CURLOPT_URL = 10002;
constexpr int CURLOPT_ERRORBUFFER = 10010;
constexpr int CURLOPT_WRITEFUNCTION = 20011;
constexpr int CURLOPT_TIMEOUT = 13;
constexpr int CURLOPT_USERAGENT = 10018;
constexpr int CURLOPT_LOW_SPEED_LIMIT = 19;
constexpr int CURLOPT_LOW_SPEED_TIME = 20;
constexpr int CURLOPT_HTTPHEADER = 10023;
constexpr int CURLOPT_SSL_VERIFYPEER = 64;
constexpr int CURLOPT_CONNECTTIMEOUT = 78;
constexpr int CURLOPT_SSL_VERIFYHOST = 81;
constexpr int CURLOPT_NOSIGNAL = 99;
constexpr int CURLOPT_NOPROGRESS = 43;
constexpr int CURLOPT_XFERINFODATA = 10057;
constexpr int CURLOPT_XFERINFOFUNCTION = 20219;
constexpr int CURLOPT_CAINFO_BLOB = 40309;
constexpr int CURLINFO_RESPONSE_CODE = 0x200002;
constexpr int CURLINFO_SSL_VERIFYRESULT = 0x20000D;
constexpr unsigned CURL_BLOB_COPY = 1;
constexpr int CURL_ERROR_SIZE = 256;

// The CURLcode values this tells apart. Everything else is printed as its number.
constexpr int CURLE_COULDNT_RESOLVE_HOST = 6;
constexpr int CURLE_COULDNT_CONNECT = 7;
constexpr int CURLE_OPERATION_TIMEDOUT = 28;
constexpr int CURLE_SSL_CONNECT_ERROR = 35;
constexpr int CURLE_SSL_CERTPROBLEM = 58;
constexpr int CURLE_SSL_CIPHER = 59;
constexpr int CURLE_PEER_FAILED_VERIFICATION = 60;
constexpr int CURLE_SSL_CACERT_BADFILE = 77;

// How far a download may be sent on. GitHub's asset link answers with one redirect, to
// objects.githubusercontent.com, so a handful is plenty and anything more is a loop.
constexpr long MAX_REDIRECTS = 5;

struct curl_blob {
    void *data;
    size_t len;
    unsigned int flags;
};

using easy_init_t = void *(*) ();
using easy_setopt_t = int (*)(void *, int, ...);
using easy_perform_t = int (*)(void *);
using easy_getinfo_t = int (*)(void *, int, ...);
using easy_cleanup_t = void (*)(void *);
using slist_append_t = void *(*) (void *, const char *);
using slist_free_t = void (*)(void *);

enum State : int {
    NOT_RUN,
    OFF,
    CHECKING,
    // The thread has its answer and is filling in the fields below. Shown as checking.
    WRITING,
    UP_TO_DATE,
    NEWER,
    NO_VERSION,
    NO_THREAD,
    // The request didn't get an answer, and `failure` says why.
    FAILED,
    UNREADABLE,
    INTERRUPTED,
};

struct Version {
    int major = -1;
    int minor = 0;
    int patch = 0;
};

bool enabled = true;

// Set once a check starts and cleared only by an interruption, so the check runs once
// per boot. Plugin memory is shared by every process, which is what lets this outlive
// the menu that set it.
bool started = false;

std::atomic<int> state{NOT_RUN};

// Which start the running thread belongs to, so a thread left over from a menu that
// closed can't write over a later one's answer.
std::atomic<int> run{0};

// Filled by the thread before it stores its state, read only after loading it.
Version running;
Version found;
UpdateFailure failure;

// What the newer release offers the installer, read out of the same answer. update.h has
// what each field means.
UpdateRelease release;

// Where the found tag's value ends in `answer`, so the release it belongs to can be read
// once the scan is over.
const char *found_end = nullptr;

// The toast, queued once from update_post. The text lives as long as the plugin, since
// the module holds it until the overlay can show it.
bool posted = false;
int toast_result = 0;
char toast_text[48];

char user_agent[48];
char error_text[CURL_ERROR_SIZE];

// "vX.Y.Z", with nothing after the third number except what stops it. Each number is at
// most five digits, which rules out anything that would overflow. Answers where it stopped,
// or nullptr when it isn't a version.
const char *parse_version(const char *at, Version *out) {
    if (*at != 'v') return nullptr;
    ++at;
    int parts[3];
    for (int i = 0; i < 3; ++i) {
        int value = 0;
        int digits = 0;
        while (*at >= '0' && *at <= '9' && digits < 6) {
            value = value * 10 + (*at - '0');
            ++at;
            ++digits;
        }
        if (digits == 0 || digits > 5) return nullptr;
        parts[i] = value;
        if (i < 2) {
            if (*at != '.') return nullptr;
            ++at;
        }
    }
    out->major = parts[0];
    out->minor = parts[1];
    out->patch = parts[2];
    return at;
}

bool newer(const Version &a, const Version &b) {
    if (a.major != b.major) return a.major > b.major;
    if (a.minor != b.minor) return a.minor > b.minor;
    return a.patch > b.patch;
}

void finish(int mine, int result) {
    if (run.load() != mine) return;
    int expected = CHECKING;
    if (!state.compare_exchange_strong(expected, WRITING)) return;
    state.store(result);
}

const char *skip_space(const char *at) {
    while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n') ++at;
    return at;
}

// Whether the release whose tag starts at `from` is a draft, going by the first "draft"
// key before the next release's tag. GitHub only shows drafts to people who can push, so
// an unauthenticated answer shouldn't carry any, but it costs nothing to be sure.
bool is_draft(const char *from) {
    const char *draft = std::strstr(from, "\"draft\"");
    if (draft == nullptr) return false;
    const char *next_tag = std::strstr(from, "\"tag_name\"");
    if (next_tag != nullptr && next_tag < draft) return false;
    const char *value = skip_space(draft + 7);
    if (*value != ':') return false;
    return std::strncmp(skip_space(value + 1), "true", 4) == 0;
}

// The highest tag on the running major line, into `found`. Answers how many tags of any
// line parsed at all, so an answer with none can be told apart from one with no newer.
int scan(const char *body) {
    int tags = 0;
    found = Version{};
    for (const char *at = std::strstr(body, "\"tag_name\""); at != nullptr;
         at = std::strstr(at, "\"tag_name\"")) {
        at += 10;
        const char *value = skip_space(at);
        if (*value != ':') continue;
        value = skip_space(value + 1);
        if (*value != '"') continue;
        Version tag;
        const char *end = parse_version(value + 1, &tag);
        if (end == nullptr || *end != '"') continue;
        if (is_draft(end)) continue;
        ++tags;
        if (tag.major == running.major && (found.major < 0 || newer(tag, found))) {
            found = tag;
            found_end = end;
        }
    }
    return tags;
}

// Where the JSON string starting at `at` closes, honoring backslash escapes, or nullptr
// when it runs past `to`, which is what a release cut off by ANSWER_MAX looks like.
const char *string_end(const char *at, const char *to) {
    while (at < to) {
        if (*at == '\\') {
            at += 2;
            continue;
        }
        if (*at == '"') return at;
        ++at;
    }
    return nullptr;
}

// The value of a string key whose name ends at `at`, or nullptr when it isn't a string.
const char *string_value(const char *at, const char *to) {
    at = skip_space(at);
    if (at >= to || *at != ':') return nullptr;
    at = skip_space(at + 1);
    if (at >= to || *at != '"') return nullptr;
    return at + 1;
}

// strstr bounded by `to`, since `answer` holds every release and only one is wanted.
const char *find_in(const char *from, const char *to, const char *needle) {
    const size_t length = std::strlen(needle);
    for (const char *at = from; at + length <= to; ++at) {
        if (std::memcmp(at, needle, length) == 0) return at;
    }
    return nullptr;
}

// The release whose tag value ends at `from`, into `release`. Its keys run from there to
// the next release's tag: GitHub lists tag_name before assets and body, and the next
// release's own keys before its tag_name are only ids and links.
//
// The asset links aren't taken from the answer. The ones a release must have are built
// from this repo and the tag, and the answer only has to list them, so nothing here can
// be pointed somewhere else by what GitHub sends back.
void read_release(const char *from) {
    release = UpdateRelease{};
    release.major = found.major;
    release.minor = found.minor;
    release.patch = found.patch;

    const char *to = std::strstr(from, "\"tag_name\"");
    if (to == nullptr) to = answer + answer_used;

    std::snprintf(release.wps_url, sizeof(release.wps_url), "%sv%d.%d.%d/%s", DOWNLOAD_BASE,
                  found.major, found.minor, found.patch, ASSET_NAME);
    std::snprintf(release.checksum_url, sizeof(release.checksum_url), "%s.sha256",
                  release.wps_url);

    for (const char *at = find_in(from, to, "\"browser_download_url\""); at != nullptr;
         at = find_in(at, to, "\"browser_download_url\"")) {
        at += 22;
        const char *value = string_value(at, to);
        if (value == nullptr) continue;
        const char *end = string_end(value, to);
        if (end == nullptr) break;
        const size_t length = static_cast<size_t>(end - value);
        if (length == std::strlen(release.wps_url) &&
            std::memcmp(value, release.wps_url, length) == 0) {
            release.has_wps = true;
        } else if (length == std::strlen(release.checksum_url) &&
                   std::memcmp(value, release.checksum_url, length) == 0) {
            release.has_checksum = true;
        }
        at = end;
    }

    // The notes, for the manual marker. A body of null has none. A body cut off by
    // ANSWER_MAX might have had one past the cut, so the release isn't offered at all.
    const char *body = find_in(from, to, "\"body\"");
    if (body == nullptr) return;
    const char *value = skip_space(body + 6);
    if (value < to && *value == ':') value = skip_space(value + 1);
    if (std::strncmp(value, "null", 4) == 0) {
        release.notes_whole = true;
        return;
    }
    value = string_value(body + 6, to);
    if (value == nullptr) return;
    const char *end = string_end(value, to);
    if (end == nullptr) return;
    release.notes_whole = true;
    release.manual = find_in(value, end, MANUAL_MARKER) != nullptr ||
                     find_in(value, end, MANUAL_MARKER_ESCAPED) != nullptr;
}

// The body as curl hands it over, already out of any chunking. What doesn't fit is
// dropped but still counted as taken, because answering less makes curl abort the
// transfer, and the releases that would be cut are the oldest ones.
size_t take_answer(char *data, size_t size, size_t count, void *) {
    const size_t length = size * count;
    const size_t room = static_cast<size_t>(ANSWER_MAX - answer_used);
    const size_t keep = length < room ? length : room;
    std::memcpy(answer + answer_used, data, keep);
    answer_used += static_cast<int>(keep);
    return length;
}

// mbedTLS's own code, from the message curl's mbedTLS layer leaves for a failed
// handshake, as in "ssl_handshake returned - mbedTLS: (-0x7200) ...". 0 when there isn't
// one.
int mbedtls_code(const char *message) {
    const char *at = std::strstr(message, "(-0x");
    if (at == nullptr) return 0;
    return -static_cast<int>(std::strtol(at + 4, nullptr, 16));
}

struct Curl {
    easy_init_t easy_init;
    easy_setopt_t easy_setopt;
    easy_perform_t easy_perform;
    easy_getinfo_t easy_getinfo;
    easy_cleanup_t easy_cleanup;
    slist_append_t slist_append;
    slist_free_t slist_free;
    curl_blob roots;
};

// Each export by name, or the name of the first one that's missing.
bool find_exports(OSDynLoad_Module module, Curl *curl, UpdateFailure *out) {
    struct Wanted {
        const char *name;
        void **slot;
    };
    void *found_at[7] = {};
    const Wanted wanted[] = {
        {"curl_easy_init", &found_at[0]},     {"curl_easy_setopt", &found_at[1]},
        {"curl_easy_perform", &found_at[2]},  {"curl_easy_getinfo", &found_at[3]},
        {"curl_easy_cleanup", &found_at[4]},  {"curl_slist_append", &found_at[5]},
        {"curl_slist_free_all", &found_at[6]},
    };
    for (const Wanted &w : wanted) {
        if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, w.name, w.slot) !=
                OS_DYNLOAD_OK ||
            *w.slot == nullptr) {
            out->fault = UpdateFault::MODULE_INCOMPLETE;
            out->missing = w.name;
            return false;
        }
    }
    curl->easy_init = reinterpret_cast<easy_init_t>(found_at[0]);
    curl->easy_setopt = reinterpret_cast<easy_setopt_t>(found_at[1]);
    curl->easy_perform = reinterpret_cast<easy_perform_t>(found_at[2]);
    curl->easy_getinfo = reinterpret_cast<easy_getinfo_t>(found_at[3]);
    curl->easy_cleanup = reinterpret_cast<easy_cleanup_t>(found_at[4]);
    curl->slist_append = reinterpret_cast<slist_append_t>(found_at[5]);
    curl->slist_free = reinterpret_cast<slist_free_t>(found_at[6]);

    // The roots: Mozilla's bundle, built into the module and exported beside curl, which
    // is what libcurlwrapper hands every handle too. Without them nothing verifies, so a
    // module that lacks them counts as incomplete rather than as a TLS failure later.
    void *pem = nullptr;
    void *pem_size = nullptr;
    if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_DATA, "cacert_pem", &pem) !=
            OS_DYNLOAD_OK ||
        OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_DATA, "cacert_pem_size", &pem_size) !=
            OS_DYNLOAD_OK ||
        pem == nullptr || pem_size == nullptr) {
        out->fault = UpdateFault::MODULE_INCOMPLETE;
        out->missing = "cacert_pem";
        return false;
    }
    curl->roots.data = pem;
    curl->roots.len = *static_cast<const uint32_t *>(pem_size);
    curl->roots.flags = CURL_BLOB_COPY;
    return true;
}

// One request's settings. The check asks for JSON and keeps its answer in memory; a
// download follows GitHub's redirect to its file host and hands each piece to the
// installer as it arrives.
struct Request {
    const char *url;
    const char *accept;
    UpdateSink sink;
    void *user;
    long max_bytes;
    long total_seconds;
    bool follow;
    const std::atomic<bool> *stop;
};

// curl's progress callback, which it calls at least once a second for as long as the
// transfer runs. Anything but 0 ends it with CURLE_ABORTED_BY_CALLBACK. The four counts
// are curl_off_t, unused here.
int keep_going(void *user, long long, long long, long long, long long) {
    const auto *stop = static_cast<const std::atomic<bool> *>(user);
    return stop != nullptr && stop->load() ? 1 : 0;
}

// One request. Answers the CURLcode, with the status in *status when it got one, or -1
// when curl couldn't make a handle.
int fetch(const Curl &curl, const Request &request, long *status, long *verify) {
    void *handle = curl.easy_init();
    if (handle == nullptr) return -1;

    void *headers = nullptr;
    if (request.accept != nullptr) headers = curl.slist_append(nullptr, request.accept);
    error_text[0] = '\0';

    // Verification stays on, against the module's roots and for the name in the URL.
    curl.easy_setopt(handle, CURLOPT_CAINFO_BLOB, &curl.roots);
    curl.easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl.easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
    curl.easy_setopt(handle, CURLOPT_URL, request.url);
    curl.easy_setopt(handle, CURLOPT_USERAGENT, user_agent);
    if (headers != nullptr) curl.easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl.easy_setopt(handle, CURLOPT_WRITEFUNCTION, request.sink);
    curl.easy_setopt(handle, CURLOPT_WRITEDATA, request.user);
    curl.easy_setopt(handle, CURLOPT_ERRORBUFFER, error_text);
    // No signals on a thread that isn't the process's own.
    curl.easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl.easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, CONNECT_SECONDS);
    curl.easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl.easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, STALL_SECONDS);
    curl.easy_setopt(handle, CURLOPT_TIMEOUT, request.total_seconds);
    // The way an ending menu stops a transfer under way.
    curl.easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &keep_going);
    curl.easy_setopt(handle, CURLOPT_XFERINFODATA, request.stop);
    curl.easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    // https and nothing else, including wherever a redirect points.
    curl.easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
    if (request.follow) {
        curl.easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
        curl.easy_setopt(handle, CURLOPT_MAXREDIRS, MAX_REDIRECTS);
        curl.easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    }
    // Refused before a byte arrives when the server says how big it is. The sink counts
    // as well, for when it doesn't.
    if (request.max_bytes > 0) curl.easy_setopt(handle, CURLOPT_MAXFILESIZE, request.max_bytes);

    const int result = curl.easy_perform(handle);
    *status = 0;
    curl.easy_getinfo(handle, CURLINFO_RESPONSE_CODE, status);
    if (result == CURLE_PEER_FAILED_VERIFICATION) {
        curl.easy_getinfo(handle, CURLINFO_SSL_VERIFYRESULT, verify);
    }
    curl.easy_cleanup(handle);
    if (headers != nullptr) curl.slist_free(headers);
    return result;
}

// Whether a finished request got its answer, and why not when it didn't.
bool transferred(int result, long status, long verify, UpdateFailure *out) {
    out->code = result;
    switch (result) {
        case 0: break;
        case CURLE_COULDNT_RESOLVE_HOST: out->fault = UpdateFault::NO_LOOKUP; return false;
        case CURLE_COULDNT_CONNECT: out->fault = UpdateFault::NO_CONNECT; return false;
        case CURLE_OPERATION_TIMEDOUT: out->fault = UpdateFault::TIMED_OUT; return false;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
            out->fault = UpdateFault::TLS_FAILED;
            out->tls_code = mbedtls_code(error_text);
            out->verify = verify;
            std::strncpy(out->detail, error_text, sizeof(out->detail) - 1);
            return false;
        default: out->fault = UpdateFault::CURL_FAILED; return false;
    }
    if (status != 200) {
        out->fault = UpdateFault::HTTP_STATUS;
        out->code = static_cast<int>(status);
        return false;
    }
    out->fault = UpdateFault::NONE;
    return true;
}

// Acquired here and released below, on this thread. The module sets curl up itself at
// every application start, so there's no global init for this side to do or undo.
// Answers false with *out filled when the module or one of its exports is missing.
bool acquire(OSDynLoad_Module *module, Curl *curl, UpdateFailure *out) {
    *module = nullptr;
    if (OSDynLoad_Acquire(CURL_MODULE, module) != OS_DYNLOAD_OK || *module == nullptr) {
        out->fault = UpdateFault::NO_MODULE;
        return false;
    }
    if (!find_exports(*module, curl, out)) {
        OSDynLoad_Release(*module);
        return false;
    }
    return true;
}

int classify(bool answered) {
    if (!answered) return FAILED;
    if (scan(answer) == 0) return UNREADABLE;
    if (found.major >= 0 && newer(found, running)) {
        read_release(found_end);
        return NEWER;
    }
    return UP_TO_DATE;
}

int check() {
    std::snprintf(user_agent, sizeof(user_agent), "JustGetMiiOnline/%s", wups_meta_version + 8);

    failure = UpdateFailure{};
    OSDynLoad_Module module;
    Curl curl{};
    if (!acquire(&module, &curl, &failure)) return FAILED;

    const Request request = {URL,          "Accept: application/vnd.github+json",
                             &take_answer, nullptr,
                             0,            TOTAL_SECONDS,
                             false,        &checker.stop};
    int result = FAILED;
    for (int i = 0; i < LOOKUP_TRIES; ++i) {
        if (i > 0 && !update_wait(&checker, LOOKUP_GAP_SECONDS * 1000)) break;
        answer_used = 0;
        long status = 0;
        long verify = 0;
        const int curl_result = fetch(curl, request, &status, &verify);
        answer[answer_used] = '\0';
        if (curl_result < 0) {
            failure.fault = UpdateFault::CURL_INIT;
            break;
        }
        result = classify(transferred(curl_result, status, verify, &failure));
        if (result != FAILED || failure.fault != UpdateFault::NO_LOOKUP) break;
        if (checker.stop.load()) break;
    }
    OSDynLoad_Release(module);
    return result;
}

void *check_thread(void *arg) {
    const int mine = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    // Stopped before the network was touched. update_app_ends has already marked it.
    if (!update_wait(&checker, delay_seconds * 1000)) return nullptr;
    const int result = check();
    LOG("update check finished in state %d, fault %d, code %d, tls %d", result,
        static_cast<int>(failure.fault), failure.code, failure.tls_code);
    finish(mine, result);
    return nullptr;
}

// mbedTLS's codes are negative hex in its headers, so they're printed the same way here
// to be looked up.
void print_code(char *out, size_t size, const char *what, int value) {
    if (value < 0) {
        std::snprintf(out, size, "%s (-0x%x)", what, static_cast<unsigned>(-value));
    } else {
        std::snprintf(out, size, "%s (%d)", what, value);
    }
}

} // namespace

namespace {

// What every thread here starts in: it notes its own OSThread, for update_stop, and runs
// the body.
void *thread_start(void *arg) {
    auto *thread = static_cast<UpdateThread *>(arg);
    thread->self.store(OSGetCurrentThread());
    return thread->body(thread->arg);
}

} // namespace

// pthread rather than std::thread, for the reason youtube/proxy.cpp gives: this build has
// no exceptions, so a std::thread that can't start calls terminate and takes the menu with
// it. Joinable, which this newlib's pthread_create makes every thread anyway, since it
// takes only the stack from its attributes. The join is what gives the stack back.
bool update_spawn(UpdateThread *thread, void *(*body)(void *), void *arg) {
    update_stop(thread);
    thread->stop.store(false);
    thread->self.store(nullptr);
    thread->body = body;
    thread->arg = arg;

    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, THREAD_STACK);
    const int rc = pthread_create(&thread->id, &attr, thread_start, thread);
    pthread_attr_destroy(&attr);
    thread->live = rc == 0;
    return thread->live;
}

void update_stop(UpdateThread *thread) {
    if (!thread->live) return;
    thread->stop.store(true);
    thread->live = false;
    if (thread->self.load() == static_cast<void *>(OSGetCurrentThread())) {
        pthread_detach(thread->id);
        return;
    }
    const OSTime from = OSGetSystemTime();
    pthread_join(thread->id, nullptr);
    LOG("update thread joined after %u ms",
        static_cast<unsigned>(OSTicksToMilliseconds(OSGetSystemTime() - from)));
}

bool update_wait(const UpdateThread *thread, int ms) {
    for (int waited = 0; waited < ms; waited += WAIT_STEP_MS) {
        if (thread->stop.load()) return false;
        OSSleepTicks(OSMillisecondsToTicks(WAIT_STEP_MS));
    }
    return !thread->stop.load();
}

bool update_get(const char *url, UpdateSink sink, void *user, long max_bytes,
                const std::atomic<bool> *stop, UpdateFailure *out) {
    *out = UpdateFailure{};
    OSDynLoad_Module module;
    Curl curl{};
    if (!acquire(&module, &curl, out)) return false;

    const Request request = {url, nullptr, sink, user, max_bytes, DOWNLOAD_SECONDS, true, stop};
    long status = 0;
    long verify = 0;
    const int result = fetch(curl, request, &status, &verify);
    OSDynLoad_Release(module);
    if (result < 0) {
        out->fault = UpdateFault::CURL_INIT;
        return false;
    }
    return transferred(result, status, verify, out);
}

void update_failure_text(const UpdateFailure &f, char *out, size_t size) {
    switch (f.fault) {
        case UpdateFault::NONE: std::snprintf(out, size, "no failure"); return;
        case UpdateFault::NO_MODULE: std::snprintf(out, size, "no CURLWrapperModule"); return;
        case UpdateFault::MODULE_INCOMPLETE:
            std::snprintf(out, size, "curl module lacks %s", f.missing);
            return;
        case UpdateFault::CURL_INIT: std::snprintf(out, size, "curl init failed"); return;
        case UpdateFault::NO_LOOKUP: std::snprintf(out, size, "no network (lookup)"); return;
        case UpdateFault::NO_CONNECT: std::snprintf(out, size, "no network (connect)"); return;
        case UpdateFault::TLS_FAILED:
            if (f.code == CURLE_PEER_FAILED_VERIFICATION) {
                std::snprintf(out, size, "TLS failed (curl %d, verify 0x%lx)", f.code, f.verify);
            } else if (f.tls_code < 0) {
                std::snprintf(out, size, "TLS failed (curl %d, -0x%x)", f.code,
                              static_cast<unsigned>(-f.tls_code));
            } else {
                std::snprintf(out, size, "TLS failed (curl %d) %s", f.code, f.detail);
            }
            return;
        case UpdateFault::TIMED_OUT: std::snprintf(out, size, "timed out"); return;
        case UpdateFault::CURL_FAILED: std::snprintf(out, size, "curl error %d", f.code); return;
        case UpdateFault::HTTP_STATUS: std::snprintf(out, size, "HTTP %d", f.code); return;
    }
    std::snprintf(out, size, "fault %d", static_cast<int>(f.fault));
}

bool update_newer(UpdateRelease *out) {
    if (state.load() != NEWER) return false;
    *out = release;
    return true;
}

void update_set_enabled(bool on) {
    enabled = on;
    int expected = on ? OFF : NOT_RUN;
    state.compare_exchange_strong(expected, on ? NOT_RUN : OFF);
}

bool update_enabled() {
    return enabled;
}

void update_start(bool at_boot) {
    if (!enabled || started || OSGetUPID() != WII_U_MENU_UPID) return;
    delay_seconds = at_boot ? BOOT_DELAY_SECONDS : START_DELAY_SECONDS;

    if (parse_version(wups_meta_version + 8, &running) == nullptr) {
        started = true;
        state.store(NO_VERSION);
        return;
    }

    started = true;
    const int mine = run.fetch_add(1) + 1;
    state.store(CHECKING);
    void *arg = reinterpret_cast<void *>(static_cast<intptr_t>(mine));
    if (!update_spawn(&checker, check_thread, arg)) {
        // Left to try again at the next foreground, since nothing went out.
        started = false;
        state.store(NO_THREAD);
    }
}

void update_post() {
    if (posted || state.load() != NEWER) return;
    posted = true;
    std::snprintf(toast_text, sizeof(toast_text), "JustGetMiiOnline v%d.%d.%d is available",
                  found.major, found.minor, found.patch);
    toast_result = notify_add(toast_text);
}

void update_app_ends() {
    if (OSGetUPID() != WII_U_MENU_UPID) return;
    // Marked first, so whatever the thread comes back with on its way out is dropped by
    // finish, and an answer it was already writing stands.
    int expected = CHECKING;
    if (state.compare_exchange_strong(expected, INTERRUPTED)) {
        started = false;
    }
    update_stop(&checker);
}

void update_status(char *out, size_t size) {
    const int now = state.load();
    switch (now) {
        case NOT_RUN: std::snprintf(out, size, "not run yet"); return;
        case OFF: std::snprintf(out, size, "off"); return;
        case CHECKING:
        case WRITING: std::snprintf(out, size, "checking"); return;
        case UP_TO_DATE:
            std::snprintf(out, size, "up to date (v%d.%d.%d)", running.major, running.minor,
                          running.patch);
            return;
        case NEWER: {
            char toast[32];
            if (!posted) {
                std::snprintf(toast, sizeof(toast), "toast waiting");
            } else if (toast_result == 0) {
                std::snprintf(toast, sizeof(toast), "toast queued");
            } else if (toast_result == NOTIFY_UNAVAILABLE) {
                std::snprintf(toast, sizeof(toast), "toast unavailable");
            } else {
                print_code(toast, sizeof(toast), "toast refused", toast_result);
            }
            std::snprintf(out, size, "v%d.%d.%d available, %s", found.major, found.minor,
                          found.patch, toast);
            return;
        }
        case NO_VERSION: std::snprintf(out, size, "own version unreadable"); return;
        case NO_THREAD: std::snprintf(out, size, "no thread, retrying"); return;
        case FAILED: update_failure_text(failure, out, size); return;
        case UNREADABLE: std::snprintf(out, size, "unreadable answer"); return;
        case INTERRUPTED: std::snprintf(out, size, "interrupted, retrying"); return;
    }
    std::snprintf(out, size, "state %d", now);
}
