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

#include "scan.h"

#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <kernel/kernel.h>

#include <cstring>

size_t swap_all(uint32_t start, uint32_t size, const void *key, const void *replacement,
                size_t width) {
    if (size < width) {
        return 0;
    }

    const uint32_t key_start = reinterpret_cast<uint32_t>(key);
    const uint32_t key_end = key_start + width;
    const uint32_t last = start + size - width;

    size_t hits = 0;

    for (uint32_t address = start; address <= last; ++address) {
        if (std::memcmp(reinterpret_cast<const void *>(address), key, width) != 0) {
            continue;
        }

        // The plugin's own copy of the pattern, found by its own scan. Overwrite
        // it and every later compare goes looking for something that's no longer
        // there.
        if (address < key_end && key_start < address + width) {
            continue;
        }

        KernelCopyData(OSEffectiveToPhysical(address),
                       OSEffectiveToPhysical(reinterpret_cast<uint32_t>(replacement)), width);

        // The write goes in through the kernel and out through a different
        // mapping, so this core's cached copy is stale until it gets pushed out.
        DCFlushRange(reinterpret_cast<void *>(address), width);

        ++hits;

        // Nothing inside a match can start another one, and the bytes now there
        // are the replacement rather than the key.
        address += width - 1;
    }

    return hits;
}
