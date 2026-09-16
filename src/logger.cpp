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

#include "logger.h"

#include <whb/log.h>
#include <whb/log_cafe.h>

#include <cstdarg>
#include <cstdio>

namespace {
bool enabled = false;
}

void log_set_enabled(bool on) {
    enabled = on;
}

bool log_enabled() {
    return enabled;
}

void log_open() {
    WHBLogCafeInit();
}

void log_close() {
    WHBLogCafeDeinit();
}

void log_line(const char *format, ...) {
    // Checked before the formatting, not after. Building the string and tossing it
    // would leave most of the cost in place, and the cost is the whole question.
    if (!enabled) {
        return;
    }

    char buffer[512];

    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    WHBLogPrintf("%s", buffer);
}
