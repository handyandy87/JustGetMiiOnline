#include "patch.h"

#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <cstring>
#include <kernel/kernel.h>
#include <whb/log.h>

#include "utils/logger.h"

// KernelCopyData works on physical addresses, so the bytes going either way pass through
// somewhere with one. A static rather than a local, since a stack address is not reliably
// translatable here, and aligned so it cannot straddle two pages and land half of itself
// somewhere else.
static uint8_t sStaging[PATCH_MAX] __attribute__((aligned(PATCH_MAX)));

uint32_t patch_find(uint32_t start, uint32_t end, const void *needle, uint32_t length,
                    uint32_t step) {
    if (!length || !step || end <= start) {
        return 0;
    }
    uint32_t page = start;
    while (page < end) {
        if (!OSIsAddressValid(page)) {
            page += OS_PAGE_SIZE;
            continue;
        }
        // A run of mapped pages, so a match lying across the join between two is still found.
        // Page by page would fault on the first unmapped one.
        uint32_t run = page;
        while (run < end && OSIsAddressValid(run)) {
            run += OS_PAGE_SIZE;
        }
        if (run - page >= length) {
            uint32_t last = run - length;
            // The needle itself is a constant in this plugin, and the plugin is linked at the
            // same addresses the app is searched in. Finding our own copy of a string reads as
            // success and patches nothing, or patches us.
            uint32_t mine = (uint32_t) needle;
            for (uint32_t at = page; at <= last; at += step) {
                if (at + length > mine && at < mine + length) {
                    continue;
                }
                if (memcmp((const void *) at, needle, length) == 0) {
                    return at;
                }
            }
        }
        page = run + OS_PAGE_SIZE;
    }
    return 0;
}

// The page an address sits on and the bytes of a run that fit on it. A run that crosses into the
// next page is copied in two, because the page after it in the address space need not be the page
// after it in memory, and the tail would land somewhere unrelated.
static uint32_t on_page(uint32_t address, uint32_t length) {
    uint32_t room = OS_PAGE_SIZE - (address & (OS_PAGE_SIZE - 1));
    return room < length ? room : length;
}

bool patch_read(uint32_t address, void *out, uint32_t length) {
    if (!length || length > PATCH_MAX) {
        return false;
    }
    uint32_t first = on_page(address, length);
    uint32_t physSrc = OSEffectiveToPhysical(address);
    uint32_t physDst = OSEffectiveToPhysical((uint32_t) sStaging);
    uint32_t physRest = 0;
    if (!physSrc || !physDst ||
        (first < length && !(physRest = OSEffectiveToPhysical(address + first)))) {
        return false;
    }
    // Written back and dropped first, so what gets read below is whatever the kernel put there
    // rather than a line this core was already holding.
    DCFlushRange(sStaging, length);
    KernelCopyData(physDst, physSrc, first);
    if (first < length) {
        KernelCopyData(physDst + first, physRest, length - first);
    }
    memcpy(out, sStaging, length);
    return true;
}

bool patch_write(uint32_t address, const void *bytes, uint32_t length) {
    if (!length || length > PATCH_MAX) {
        DEBUG_FUNCTION_LINE("WiiULeanback: %u bytes is not a size this can write", length);
        return false;
    }
    uint32_t first = on_page(address, length);
    uint32_t physDst = OSEffectiveToPhysical(address);
    uint32_t physSrc = OSEffectiveToPhysical((uint32_t) sStaging);
    uint32_t physRest = 0;
    if (!physDst || !physSrc ||
        (first < length && !(physRest = OSEffectiveToPhysical(address + first)))) {
        DEBUG_FUNCTION_LINE("WiiULeanback: 0x%08x has no physical address, left alone", address);
        return false;
    }
    memcpy(sStaging, bytes, length);
    // The kernel reads this out of memory rather than out of our cache, so it has to be there
    // before the copy runs.
    DCFlushRange(sStaging, length);
    // A line still dirty for the address would be written back over the copy later, so it goes
    // before the copy and again after, which keeps the new bytes whichever way the kernel wrote
    // them. Invalidating instead would throw away a copy that went in through the cache.
    DCFlushRange((void *) address, length);
    KernelCopyData(physDst, physSrc, first);
    if (first < length) {
        KernelCopyData(physRest, physSrc + first, length - first);
    }
    DCFlushRange((void *) address, length);
    // And the core must not keep running an instruction it already fetched from here.
    ICInvalidateRange((void *) address, length);

    return memcmp((const void *) address, bytes, length) == 0;
}
