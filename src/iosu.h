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

// Reaching IOSU, and nothing about what is worth writing there.
//
// These were account.cpp's private helpers until SpotPass needed the same four. They
// live here rather than in two copies because a second copy of a routine that pokes
// the security processor is the kind of duplication that gets fixed in one place and
// not the other. Every policy decision stayed where it was: which address, what proof
// is demanded before writing, and what the menu is told all still live with the
// setting they belong to.

#include <cstddef>
#include <cstdint>

// Whether Mocha is installed at all, asked once when the plugin initializes. Failing
// here costs the settings and nothing else.
bool iosu_init();
void iosu_shutdown();
bool iosu_ready();

// A handle to Mocha, opened in the calling process when this is constructed and closed
// when it goes out of scope.
//
// Open one for a whole operation and hand it to every read and write that operation
// makes. Not one kept from plugin init: that handle belongs to the first Wii U Menu and
// to no process after it, which iosu.cpp goes into. And not one per read either, which
// bolts an open and a close onto every word and puts an open that can fail in front of
// an operation's last write, often the one that puts something back.
class IosuHandle {
public:
    IosuHandle();
    ~IosuHandle();

    IosuHandle(const IosuHandle &) = delete;
    IosuHandle &operator=(const IosuHandle &) = delete;

    // False when this process couldn't open Mocha. Every call below then fails
    // without sending anything.
    bool is_open() const;

    // A word at a time is the only access there is, so every read and write below is
    // a run of them.
    bool read_bytes(uint32_t address, void *out, size_t length) const;

    // Writes exactly `length` bytes and no more.
    //
    // The last word usually straddles the end of the run, so it's read first and the
    // bytes past it keep whatever IOSU had there. That's not tidiness: it's what
    // leaves the tail of a longer original intact underneath a shorter replacement,
    // which is how spotpass.cpp can later prove how wide a slot really is.
    bool write_bytes(uint32_t address, const void *data, size_t length) const;

    // One whole word, for the one caller that pokes a value rather than a string: the
    // BOSS policy refresh in spotpass.cpp.
    bool write_word(uint32_t address, uint32_t value) const;

    // Whether `length` bytes at the address already equal the image. Callers use it
    // to make a second application start do no work and say nothing.
    bool matches(uint32_t address, const void *image, size_t length) const;

private:
    bool read_word(uint32_t address, uint32_t &out) const;

    const int32_t handle;
};
