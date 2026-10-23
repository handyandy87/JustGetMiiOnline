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

#include "update_install.h"
#include "logger.h"
#include "notify.h"
#include "sha256.h"
#include "update.h"

#include <coreinit/debug.h>
#include <coreinit/launch.h>
#include <sysapp/launch.h>

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

#define PLUGIN_PATH "fs:/vol/external01/wiiu/environments/aroma/plugins/JustGetMiiOnline.wps"
constexpr const char INSTALLED[] = PLUGIN_PATH;
constexpr const char PART[] = PLUGIN_PATH ".part";
constexpr const char BACKUP[] = PLUGIN_PATH ".bkup";

// Every release so far is under 200 KB. Ten times that is room to grow, and still small
// enough that a link answering with something else entirely is cut off early.
constexpr long MAX_PLUGIN_BYTES = 2 * 1024 * 1024;

// One line of sha256sum: 64 hex digits, two spaces, the name, a newline.
constexpr long MAX_CHECKSUM_BYTES = 256;

// The menu process. update.cpp and plaza.cpp key on the same number.
constexpr uint32_t WII_U_MENU_UPID = 2;

enum State : int {
    IDLE,
    DOWNLOADING,
    // Between the two renames. The menu closing now would leave no plugin at all, so
    // nothing here looks at the stop flag, and update_install_app_ends's join waits it out.
    SWAPPING,
    RELAUNCH_WAITING,
    RELAUNCHING,
    RESTART_NEEDED,
    NO_THREAD,
    INTERRUPTED,
    NOT_INSTALLED_HERE,
    CHECKSUM_FETCH,
    CHECKSUM_UNREADABLE,
    PLUGIN_FETCH,
    TOO_BIG,
    NOT_PLUGIN,
    MISMATCH,
    WRITE_FAILED,
    SWAP_FAILED,
    RESTORE_FAILED,
};

std::atomic<int> state{IDLE};

// The install's thread. update_install_app_ends stops and joins it.
UpdateThread installer;

bool failed(int now) {
    return now >= NO_THREAD;
}

// Set from the menu, read by the thread, which starts after both are written.
UpdateRelease target;
bool chosen = false;

// What went wrong, filled in by the thread before it stores the state that names it.
UpdateFailure failure;
int error_number = 0;
int restore_error = 0;

// Whether the Wii U Menu is in front, and whether a relaunch is owed. The claim makes
// sure the thread and the hook can't both make it.
std::atomic<bool> menu_foreground{false};
std::atomic<bool> relaunch_wanted{false};
std::atomic<bool> relaunch_claimed{false};

// The toasts. Their text lives as long as the plugin, since the module keeps the pointer
// until the overlay shows it.
char start_text[48];
char end_text[64];
std::atomic<bool> end_posted{false};

// The .sha256 file as it arrives.
char checksum_text[MAX_CHECKSUM_BYTES + 1];
int checksum_used = 0;
bool checksum_overflow = false;

// The download's own buffer for the SD card, so a write is one 32 KB call and not one per
// piece curl hands over. Static, like the check's answer, so the thread's stack is left
// to curl.
char file_buffer[32 * 1024];

struct Download {
    std::FILE *file;
    Sha256 sha;
    long received;
    uint8_t head[20];
    bool too_big;
    bool not_plugin;
    bool write_failed;
    int error;
};

// Whether the first 20 bytes are an Aroma plugin's: a 32-bit big-endian ELF for Cafe OS
// (ABI 0xCA, version 0xFE), the "PL" the plugin system's build rules stamp at offset 9,
// RPL type 0xFE01, PowerPC. Every release so far starts with exactly these bytes. It's not
// a check that the plugin loads, only that the file isn't a web page or a truncated reply,
// which is what a link that went wrong gives back.
bool looks_like_plugin(const uint8_t *h) {
    static constexpr uint8_t ident[11] = {0x7f, 'E', 'L', 'F', 1, 2, 1, 0xca, 0xfe, 'P', 'L'};
    return std::memcmp(h, ident, sizeof(ident)) == 0 && h[16] == 0xfe && h[17] == 0x01 &&
           h[18] == 0x00 && h[19] == 0x14;
}

size_t take_checksum(char *data, size_t size, size_t count, void *) {
    const size_t length = size * count;
    if (static_cast<size_t>(checksum_used) + length > MAX_CHECKSUM_BYTES) {
        checksum_overflow = true;
        return 0;
    }
    std::memcpy(checksum_text + checksum_used, data, length);
    checksum_used += static_cast<int>(length);
    return length;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// sha256sum's line for the release's .wps: the digest, a space, a space or the '*' that
// marks binary mode, and the file's name, which has to be this one. Anything after the
// name but a line ending is refused, since a checksum file for some other file is the
// wrong file however well formed it is.
bool parse_checksum(const char *text, uint8_t digest[32]) {
    for (int i = 0; i < 32; ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        digest[i] = static_cast<uint8_t>(high * 16 + low);
    }
    const char *at = text + 64;
    if (*at != ' ') return false;
    ++at;
    if (*at == ' ' || *at == '*') ++at;
    constexpr const char name[] = "JustGetMiiOnline.wps";
    if (std::strncmp(at, name, sizeof(name) - 1) != 0) return false;
    at += sizeof(name) - 1;
    while (*at == '\r' || *at == '\n') ++at;
    return *at == '\0';
}

// Each piece goes three places: the header check, the hash and the file. A short answer
// makes curl stop, and the flag says which of the three wanted it to.
size_t take_plugin(char *data, size_t size, size_t count, void *user) {
    auto *d = static_cast<Download *>(user);
    const size_t length = size * count;
    if (d->received + static_cast<long>(length) > MAX_PLUGIN_BYTES) {
        d->too_big = true;
        return 0;
    }

    const long head_size = static_cast<long>(sizeof(d->head));
    if (d->received < head_size) {
        const size_t room = static_cast<size_t>(head_size - d->received);
        const size_t take = length < room ? length : room;
        std::memcpy(d->head + d->received, data, take);
        if (d->received + static_cast<long>(take) == head_size && !looks_like_plugin(d->head)) {
            d->not_plugin = true;
            return 0;
        }
    }

    sha256_update(&d->sha, data, length);
    if (std::fwrite(data, 1, length, d->file) != length) {
        d->write_failed = true;
        d->error = errno;
        return 0;
    }
    d->received += static_cast<long>(length);
    return length;
}

// The new file, whole and checked, at PART. Answers 0 or the failure's state.
int download(const uint8_t want[32]) {
    std::remove(PART);
    std::FILE *file = std::fopen(PART, "wb");
    if (file == nullptr) {
        error_number = errno;
        return WRITE_FAILED;
    }
    std::setvbuf(file, file_buffer, _IOFBF, sizeof(file_buffer));

    Download d{};
    d.file = file;
    sha256_init(&d.sha);
    const bool answered =
        update_get(target.wps_url, &take_plugin, &d, MAX_PLUGIN_BYTES, &installer.stop, &failure);

    // curl's own size refusal, for when GitHub's file host said up front how big it was.
    constexpr int CURLE_FILESIZE_EXCEEDED = 63;
    int result = 0;
    if (d.too_big ||
        (failure.fault == UpdateFault::CURL_FAILED && failure.code == CURLE_FILESIZE_EXCEEDED)) {
        result = TOO_BIG;
    } else if (d.not_plugin) {
        result = NOT_PLUGIN;
    } else if (d.write_failed) {
        error_number = d.error;
        result = WRITE_FAILED;
    } else if (!answered) {
        result = PLUGIN_FETCH;
    } else if (d.received < static_cast<long>(sizeof(d.head))) {
        result = NOT_PLUGIN;
    }

    // Flushed and synced before it's closed, so what gets renamed into place is on the
    // card and not in a buffer.
    if (result == 0 && (std::fflush(file) != 0 || fsync(fileno(file)) != 0)) {
        error_number = errno;
        result = WRITE_FAILED;
    }
    if (std::fclose(file) != 0 && result == 0) {
        error_number = errno;
        result = WRITE_FAILED;
    }

    if (result == 0) {
        uint8_t got[32];
        sha256_final(&d.sha, got);
        if (std::memcmp(got, want, sizeof(got)) != 0) result = MISMATCH;
    }
    if (result != 0) std::remove(PART);
    return result;
}

// The two renames, with the installed file kept as BACKUP. FAT won't rename onto a name
// that exists, so an older backup goes first. Answers RELAUNCH_WAITING or the failure.
int swap() {
    std::remove(BACKUP);
    if (std::rename(INSTALLED, BACKUP) != 0) {
        error_number = errno;
        std::remove(PART);
        return SWAP_FAILED;
    }
    if (std::rename(PART, INSTALLED) != 0) {
        error_number = errno;
        if (std::rename(BACKUP, INSTALLED) != 0) {
            restore_error = errno;
            return RESTORE_FAILED;
        }
        std::remove(PART);
        return SWAP_FAILED;
    }
    return RELAUNCH_WAITING;
}

// The same two calls, in the same order, that the settings use in config.cpp, which has
// why both. Only from the Wii U Menu while it's in front, and only once.
//
// A failed install relaunches too when a setting changed in the same visit asked for it.
// An interrupted one never does, and neither does one already turned into a restart.
void relaunch_if_ready() {
    if (!relaunch_wanted.load() || !menu_foreground.load()) return;
    if (OSGetUPID() != WII_U_MENU_UPID) return;
    const int now = state.load();
    if (now != RELAUNCH_WAITING && !(failed(now) && now != INTERRUPTED)) return;
    if (relaunch_claimed.exchange(true)) return;
    int expected = RELAUNCH_WAITING;
    state.compare_exchange_strong(expected, RELAUNCHING);
    LOG("relaunching to load the installed update");
    OSForceFullRelaunch();
    SYSLaunchMenu();
}

int install() {
    struct stat st;
    if (stat(INSTALLED, &st) != 0) {
        error_number = errno;
        return NOT_INSTALLED_HERE;
    }
    if (!S_ISREG(st.st_mode)) return NOT_INSTALLED_HERE;

    checksum_used = 0;
    checksum_overflow = false;
    const bool answered =
        update_get(target.checksum_url, &take_checksum, nullptr, MAX_CHECKSUM_BYTES,
                   &installer.stop, &failure);
    checksum_text[checksum_used] = '\0';
    if (checksum_overflow) return CHECKSUM_UNREADABLE;
    if (!answered) return CHECKSUM_FETCH;
    uint8_t want[32];
    if (!parse_checksum(checksum_text, want)) return CHECKSUM_UNREADABLE;

    if (const int result = download(want); result != 0) return result;

    // The menu may have closed while the download ran, which update_install_app_ends
    // marks. Whatever this thread is still doing then, it doesn't touch the plugin.
    int expected = DOWNLOADING;
    if (!state.compare_exchange_strong(expected, SWAPPING)) {
        std::remove(PART);
        return INTERRUPTED;
    }
    return swap();
}

void *install_thread(void *) {
    const int result = install();
    LOG("update install finished in state %d, errno %d, fault %d, code %d", result,
        error_number, static_cast<int>(failure.fault), failure.code);

    // Wanted before the state says so, so a menu closing in between turns it into a
    // restart instead of finding a relaunch owed with nothing waiting. Stored only over
    // the thread's own states, so an interruption the hook already recorded stands.
    if (result == RELAUNCH_WAITING) relaunch_wanted.store(true);
    int expected = state.load();
    if (expected == DOWNLOADING || expected == SWAPPING) {
        state.compare_exchange_strong(expected, result);
    }
    relaunch_if_ready();

    // A failure is told right away rather than at the next hook: closing the config menu
    // brings no foreground change, so the hook could be minutes off. AromaBasePlugin posts
    // its own toast from its check thread the same way. Not while the menu is ending,
    // though: the next hook posts it, in a process that's starting rather than going.
    if (!installer.stop.load() && failed(state.load())) update_install_post();
    return nullptr;
}

bool installable(const UpdateRelease &r) {
    return r.has_wps && r.has_checksum && r.notes_whole && !r.manual;
}

bool in_progress(int now) {
    return now == DOWNLOADING || now == SWAPPING || now == RELAUNCH_WAITING ||
           now == RELAUNCHING;
}

} // namespace

InstallOffer update_install_offer(char *text, size_t size) {
    UpdateRelease r;
    if (!update_newer(&r)) return InstallOffer::NONE;
    const int now = state.load();
    if (in_progress(now)) {
        std::snprintf(text, size, "Installing v%d.%d.%d, see the Debug page", r.major, r.minor,
                      r.patch);
        return InstallOffer::LINE;
    }
    if (now == RESTART_NEEDED) {
        std::snprintf(text, size, "Restart your console to finish updating");
        return InstallOffer::LINE;
    }
    if (!installable(r)) {
        std::snprintf(text, size, "v%d.%d.%d needs a manual install, see the Releases page",
                      r.major, r.minor, r.patch);
        return InstallOffer::LINE;
    }
    // Like the plaza reset, only from the Wii U Menu. A relaunch from inside a game would
    // throw away whatever the game hadn't saved, and the check only ever runs there anyway.
    if (OSGetUPID() != WII_U_MENU_UPID) {
        std::snprintf(text, size, "Install v%d.%d.%d from the Wii U Menu", r.major, r.minor,
                      r.patch);
        return InstallOffer::LINE;
    }
    std::snprintf(text, size, "Install v%d.%d.%d", r.major, r.minor, r.patch);
    return InstallOffer::ITEM;
}

void update_install_choose(bool yes) {
    chosen = yes;
}

bool update_install_begin(bool relaunch_after) {
    if (!chosen) return false;
    chosen = false;

    UpdateRelease r;
    const int now = state.load();
    if (OSGetUPID() != WII_U_MENU_UPID || !update_newer(&r) || !installable(r) ||
        in_progress(now) || now == RESTART_NEEDED) {
        return false;
    }

    target = r;
    failure = UpdateFailure{};
    error_number = 0;
    restore_error = 0;
    end_posted = false;
    // The menu closing is the Wii U Menu's, so it's in front now. The hooks keep this
    // right from here on.
    menu_foreground.store(true);
    relaunch_claimed.store(false);
    relaunch_wanted.store(relaunch_after);
    state.store(DOWNLOADING);

    if (!update_spawn(&installer, install_thread, nullptr)) {
        state.store(NO_THREAD);
        relaunch_wanted.store(false);
        return false;
    }

    std::snprintf(start_text, sizeof(start_text), "Downloading JustGetMiiOnline v%d.%d.%d",
                  r.major, r.minor, r.patch);
    notify_add(start_text);
    return true;
}

void update_install_foreground(bool acquired) {
    if (OSGetUPID() != WII_U_MENU_UPID) return;
    menu_foreground.store(acquired);
    if (acquired) relaunch_if_ready();
}

void update_install_post() {
    if (end_posted.load()) return;
    const int now = state.load();
    if (now == RESTART_NEEDED) {
        std::snprintf(end_text, sizeof(end_text),
                      "Restart your console to finish updating JustGetMiiOnline");
    } else if (failed(now)) {
        std::snprintf(end_text, sizeof(end_text),
                      "JustGetMiiOnline: Update Failed. Visit GitHub.");
    } else {
        return;
    }
    if (end_posted.exchange(true)) return;
    notify_add(end_text);
}

void update_install_app_ends() {
    if (OSGetUPID() != WII_U_MENU_UPID) return;
    menu_foreground.store(false);

    // Whatever happens below, nothing relaunches into what's starting now. A setting
    // changed alongside an install that didn't finish waits for the next boot, the way
    // settings did before the menu relaunched for them.
    //
    // A download under way is marked first, so whatever the thread comes back with once
    // the stop cuts it off is dropped, and it never reaches the swap.
    int expected = DOWNLOADING;
    if (state.compare_exchange_strong(expected, INTERRUPTED)) relaunch_wanted.store(false);

    // A swap under way finishes before this returns, and so does a relaunch the thread is
    // making. update_install.h has why that join can't be the thread waiting on itself.
    update_stop(&installer);

    // The files are in place, and whatever is starting now isn't something to relaunch
    // out of. The next boot loads the new one.
    expected = RELAUNCH_WAITING;
    state.compare_exchange_strong(expected, RESTART_NEEDED);
    relaunch_wanted.store(false);
}

void update_install_status(char *out, size_t size) {
    UpdateRelease r;
    const bool have = update_newer(&r);
    const int now = state.load();
    char why[64];
    switch (now) {
        case IDLE:
            if (!have) {
                std::snprintf(out, size, "not offered");
            } else if (r.manual) {
                std::snprintf(out, size, "v%d.%d.%d manual-only", r.major, r.minor, r.patch);
            } else if (!r.notes_whole) {
                std::snprintf(out, size, "not offered, release notes cut off");
            } else if (!r.has_wps) {
                std::snprintf(out, size, "not offered, no .wps asset");
            } else if (!r.has_checksum) {
                std::snprintf(out, size, "not offered, no .sha256 asset");
            } else {
                std::snprintf(out, size, "offered v%d.%d.%d", r.major, r.minor, r.patch);
            }
            return;
        case DOWNLOADING:
            std::snprintf(out, size, "downloading v%d.%d.%d", target.major, target.minor,
                          target.patch);
            return;
        case SWAPPING: std::snprintf(out, size, "swapping"); return;
        case RELAUNCH_WAITING: std::snprintf(out, size, "swapped, relaunch waits for menu"); return;
        case RELAUNCHING: std::snprintf(out, size, "swapped, relaunching"); return;
        case RESTART_NEEDED: std::snprintf(out, size, "swapped, restart needed"); return;
        case NO_THREAD: std::snprintf(out, size, "no thread"); return;
        case INTERRUPTED: std::snprintf(out, size, "interrupted, old one kept"); return;
        case NOT_INSTALLED_HERE:
            std::snprintf(out, size, "no plugins/JustGetMiiOnline.wps (errno %d)", error_number);
            return;
        case CHECKSUM_FETCH:
            update_failure_text(failure, why, sizeof(why));
            std::snprintf(out, size, ".sha256 %s", why);
            return;
        case CHECKSUM_UNREADABLE: std::snprintf(out, size, ".sha256 unreadable"); return;
        case PLUGIN_FETCH:
            update_failure_text(failure, why, sizeof(why));
            std::snprintf(out, size, ".wps %s", why);
            return;
        case TOO_BIG: std::snprintf(out, size, "larger than 2 MB"); return;
        case NOT_PLUGIN: std::snprintf(out, size, "not a plugin"); return;
        case MISMATCH: std::snprintf(out, size, "checksum mismatch"); return;
        case WRITE_FAILED:
            std::snprintf(out, size, "write failed (errno %d)", error_number);
            return;
        case SWAP_FAILED:
            std::snprintf(out, size, "swap failed (errno %d), old one kept", error_number);
            return;
        case RESTORE_FAILED:
            std::snprintf(out, size, "swap failed (errno %d), restore failed (errno %d)",
                          error_number, restore_error);
            return;
    }
    std::snprintf(out, size, "state %d", now);
}
