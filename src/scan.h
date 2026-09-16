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

// Find a run of bytes and write another run of the same length over it. The write
// goes through the kernel because an ordinary store can't touch the memory this
// targets.
//
// Equal lengths is the rule that keeps this safe, and the signature enforces it:
// one width governs both the compare and the write, so an overrun isn't even
// expressible. Every caller here matches a whole 39-byte string slot or a whole
// 277-byte record and drops the same number of bytes back in.
//
// Returns how many it replaced, not whether it replaced one, and doesn't stop at the
// first match: a count of zero and a count of two are different faults, and a bool
// hides both.
//
// `key` is where the pattern lives in this plugin's own memory. Hits that overlap it
// get skipped, since a plugin is loaded somewhere inside the range being searched,
// and a scan that finds its own search key clobbers it along with every iteration
// after.
size_t swap_all(uint32_t start, uint32_t size, const void *key, const void *replacement,
                size_t width);
