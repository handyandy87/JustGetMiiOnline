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

namespace {

// The bytes of a run that fit on the page an address sits on. The page after it in the address
// space need not be the page after it in memory, so a run that crosses into the next one is
// copied in two and its tail is translated on its own.
uint32_t on_page(uint32_t address, uint32_t length) {
    const uint32_t room = OS_PAGE_SIZE - (address & (OS_PAGE_SIZE - 1));
    return room < length ? room : length;
}

bool translates(uint32_t address, uint32_t length) {
    for (uint32_t done = 0; done < length; done += on_page(address + done, length - done)) {
        if (OSEffectiveToPhysical(address + done) == 0) {
            return false;
        }
    }
    return true;
}

// One write through the kernel, which copies between physical addresses and so goes around
// this core's cache rather than through it.
//
// So there are three things to get right, and flushing the target afterwards is only one. The
// replacement has to be in memory before the copy reads it, so it gets flushed first. A line
// still dirty for the target would be written back over the copy at some later eviction, so
// the target is flushed before the copy as well as after it; invalidating instead would throw
// away bytes the kernel might have put in through the cache. And a core that already fetched
// instructions from the target must not keep running them, which only matters for the text
// ranges the launch-time sweep still covers, and costs nothing for the rest.
//
// Every piece is translated before any of them is written, so a write either goes in whole or
// not at all.
bool kernel_write(uint32_t address, const void *bytes, uint32_t width) {
    const uint32_t source = reinterpret_cast<uint32_t>(bytes);
    if (!translates(address, width) || !translates(source, width)) {
        return false;
    }

    DCFlushRange(const_cast<void *>(bytes), width);
    DCFlushRange(reinterpret_cast<void *>(address), width);

    uint32_t done = 0;
    while (done < width) {
        uint32_t piece = on_page(address + done, width - done);
        piece = on_page(source + done, piece);
        KernelCopyData(OSEffectiveToPhysical(address + done), OSEffectiveToPhysical(source + done),
                       piece);
        done += piece;
    }

    DCFlushRange(reinterpret_cast<void *>(address), width);
    ICInvalidateRange(reinterpret_cast<void *>(address), width);
    return true;
}

} // namespace

size_t swap_all(uint32_t start, uint32_t size, const void *key, const void *replacement,
                size_t width, size_t *read_back) {
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

        // A hit with no physical address is left as it was and not counted, since nothing
        // was written to it.
        if (!kernel_write(address, replacement, static_cast<uint32_t>(width))) {
            continue;
        }

        ++hits;

        // Read through the ordinary mapping, after the flush dropped this core's line, so it
        // says what the library itself will read.
        if (read_back != nullptr &&
            std::memcmp(reinterpret_cast<const void *>(address), replacement, width) == 0) {
            ++*read_back;
        }

        // Nothing inside a match can start another one, and the bytes now there
        // are the replacement rather than the key.
        address += width - 1;
    }

    return hits;
}
