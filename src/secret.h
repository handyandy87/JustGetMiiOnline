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

#include <cstddef>
#include <cstdint>

// Project Rose's obfuscation constant, read off the console from their own module
// rather than compiled into this one. The user keeps a copy of that module on the SD
// card and an action in the menu imports the constant out of it once and stores it.
// Nothing here ever ships the value.

// The length isn't a property of the string, which is why I check it against the size
// the symbol records. An import that took the bytes without checking would mint a
// token that fails with nothing on screen to explain it, so a size that isn't
// SECRET_STORED gets refused.
constexpr size_t SECRET_LEN = 16;
constexpr size_t SECRET_STORED = SECRET_LEN + 1; // the array includes the terminator

// What the menu shows. Three values and no fourth: either nothing has been
// imported, something valid has, or the last attempt didn't produce one.
//
// Failed is deliberately not the same as NotImported. A console that has never
// run the action and one whose last run was pointed at the wrong file are in
// different situations and need different things done to them, and folding both
// into "nothing here" would hide which.
enum class ImportState : uint32_t {
    NotImported = 0,
    Imported = 1,
    Failed = 2,
};

// The three spellings, in one place rather than in a switch at each site.
constexpr const char *import_state_name(ImportState state) {
    switch (state) {
        case ImportState::NotImported: return "Not Imported";
        case ImportState::Imported: return "Imported";
        case ImportState::Failed: return "Import Failed";
    }
    return "Not Imported";
}

// Where the user is asked to put their copy. Deliberately not the modules folder:
// a second module exporting the same three symbols under the same name as the one
// already installed is the collision this whole approach exists to avoid.
constexpr const char *SECRET_SOURCE_PATH = "fs:/vol/external01/wiiu/roseverse.wms";

// Read storage into the process global. Called from Config::Init, where storage is
// readable, because the applet process can't read it at all.
void secret_init();

// Do the import. Reads SECRET_SOURCE_PATH, checks it, stores on success. Returns
// the state to display, and leaves any constant already stored alone when the
// attempt fails: a bad run shouldn't cost a good value.
ImportState secret_import();

// What the menu shows now, and why, if the last attempt failed. The reason is a
// sentence rather than a code because it's the only place the user is told what to
// do differently, and every failure calls for a different action.
ImportState secret_state();
const char *secret_failure();

// The constant itself, for the token hook. Null unless a valid one is in hand,
// which is the gate: with no constant the hook declines every request it sees, so
// an absent or failed import leaves the console doing exactly what it does today.
const uint8_t *secret_bytes();
