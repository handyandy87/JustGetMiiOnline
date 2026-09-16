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

#include "rosepatcher.h"
#include "logger.h"

#include <cstdio>
#include <cstring>

namespace {

// RosePatcher's settings file. The backend names it after the plugin's storage id,
// rosepatcher, in the plugin folder's config directory. Aroma is the one environment the
// README installs into, so the folder is Aroma's.
constexpr const char *CONFIG_PATH =
    "fs:/vol/external01/wiiu/environments/aroma/plugins/config/rosepatcher.json";

// Quotes included, so neither key can match inside the other or inside a value.
constexpr const char *KEY_ROSEVERSE = "\"connect_to_roseverse\"";
constexpr const char *KEY_NEWS = "\"connect_to_rose_news\"";

// The file is a few hundred bytes. One bigger than this isn't the file I expect, and it
// gets left alone rather than read in part and written back short.
constexpr size_t CONFIG_MAX = 4096;

enum class State {
    NotChecked,
    NotInstalled,
    Unreadable,
    NoOptions,
    InStep,
    OutOfStep,
};

State state = State::NotChecked;
bool sync_armed = false;
char status_line[96] = "not checked yet";

// The two values this plugin wants for a given Miiverse.
bool wants_roseverse(Miiverse miiverse) {
    return miiverse == Miiverse::Roseverse;
}

constexpr bool WANTS_NEWS = false;

// The whole file into `buffer`, terminated. False when it isn't there, can't be read, or
// is too big to be the file I expect, with `missing` saying which of those it was.
bool read_config(char *buffer, size_t &length, bool &missing) {
    length = 0;
    missing = false;

    std::FILE *file = std::fopen(CONFIG_PATH, "rb");
    if (file == nullptr) {
        // Not being able to open it is read as RosePatcher never having saved anything
        // here, which is the ordinary case on a console without it. An open that fails
        // for some other reason reads the same, and then the options just aren't
        // touched.
        missing = true;
        return false;
    }

    length = std::fread(buffer, 1, CONFIG_MAX, file);
    const bool whole = std::feof(file) != 0 && std::ferror(file) == 0 && length < CONFIG_MAX;
    std::fclose(file);
    if (!whole) {
        return false;
    }
    buffer[length] = '\0';
    return true;
}

// Where a key's true or false literal sits. The backend writes these as a quoted key, a
// colon and a bare literal, with its own spacing around the colon, so spacing is skipped
// and nothing else is.
bool find_flag(const char *buffer, const char *key, size_t &at, size_t &width, bool &value) {
    const char *found = std::strstr(buffer, key);
    if (found == nullptr) {
        return false;
    }

    const char *p = found + std::strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != ':') {
        return false;
    }
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;

    if (std::strncmp(p, "true", 4) == 0) {
        value = true;
        width = 4;
    } else if (std::strncmp(p, "false", 5) == 0) {
        value = false;
        width = 5;
    } else {
        return false;
    }
    at = static_cast<size_t>(p - buffer);
    return true;
}

// Put `wanted` in place of the key's literal. true and false differ in length by a byte,
// so the rest of the file moves with it, inside a buffer with room for the one byte more.
bool set_flag(char *buffer, size_t &length, const char *key, bool wanted, bool &changed) {
    size_t at = 0;
    size_t width = 0;
    bool value = false;
    if (!find_flag(buffer, key, at, width, value)) {
        return false;
    }
    if (value == wanted) {
        return true;
    }

    const char *literal = wanted ? "true" : "false";
    const size_t literal_width = wanted ? 4 : 5;
    const size_t new_length = length - width + literal_width;
    if (new_length >= CONFIG_MAX) {
        return false;
    }

    std::memmove(buffer + at + literal_width, buffer + at + width, length - at - width + 1);
    std::memcpy(buffer + at, literal, literal_width);
    length = new_length;
    changed = true;
    return true;
}

void set_status(const char *text) {
    std::snprintf(status_line, sizeof(status_line), "%s", text);
}

} // namespace

void rosepatcher_check(Miiverse miiverse) {
    char buffer[CONFIG_MAX + 1];
    size_t length = 0;
    bool missing = false;

    if (!read_config(buffer, length, missing)) {
        state = missing ? State::NotInstalled : State::Unreadable;
        set_status(missing ? "no config found, nothing to set" : "config unreadable, left alone");
        return;
    }

    size_t at = 0;
    size_t width = 0;
    bool roseverse = false;
    bool news = false;
    if (!find_flag(buffer, KEY_ROSEVERSE, at, width, roseverse) ||
        !find_flag(buffer, KEY_NEWS, at, width, news)) {
        state = State::NoOptions;
        set_status("options not in its config, left alone");
        return;
    }

    if (roseverse == wants_roseverse(miiverse) && news == WANTS_NEWS) {
        state = State::InStep;
        set_status("in step");
        return;
    }

    state = State::OutOfStep;
    std::snprintf(status_line, sizeof(status_line), "Roseverse %s, News %s, fixed at relaunch",
                  roseverse ? "on" : "off", news ? "on" : "off");
}

bool rosepatcher_found() {
    return state == State::InStep || state == State::OutOfStep;
}

bool rosepatcher_out_of_step() {
    return state == State::OutOfStep;
}

void rosepatcher_arm_sync() {
    sync_armed = true;
}

void rosepatcher_sync(Miiverse miiverse) {
    if (!sync_armed) {
        return;
    }
    sync_armed = false;

    char buffer[CONFIG_MAX + 1];
    size_t length = 0;
    bool missing = false;
    if (!read_config(buffer, length, missing)) {
        LOG("RosePatcher options: config %s at exit", missing ? "gone" : "unreadable");
        return;
    }

    bool changed = false;
    if (!set_flag(buffer, length, KEY_ROSEVERSE, wants_roseverse(miiverse), changed) ||
        !set_flag(buffer, length, KEY_NEWS, WANTS_NEWS, changed)) {
        LOG("RosePatcher options: not in its config at exit, left alone");
        return;
    }
    if (!changed) {
        return;
    }

    std::FILE *file = std::fopen(CONFIG_PATH, "wb");
    if (file == nullptr) {
        LOG("RosePatcher options: could not open its config to write");
        return;
    }
    const size_t written = std::fwrite(buffer, 1, length, file);
    const bool closed = std::fclose(file) == 0;
    if (written != length || !closed) {
        LOG("RosePatcher options: write came up short, %u of %u bytes",
            static_cast<unsigned>(written), static_cast<unsigned>(length));
        return;
    }
    LOG("RosePatcher options: Roseverse %s, News off", wants_roseverse(miiverse) ? "on" : "off");
}

const char *rosepatcher_status() {
    return status_line;
}
