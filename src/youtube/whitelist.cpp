// The shell refuses any request the page makes to a host that is not on a list it carries, and
// beacons the refusal as steel_whitelist. The list is an array of const char* in .data pointing
// at strings in .rodata, forty odd entries, and it allows everything this revival needs except
// one: sign-in.
//
//     *.youtube.com   youtubei.googleapis.com   www.googleapis.com   *.ytimg.com
//     *.googlevideo.com   yt3.ggpht.com   yt4.ggpht.com   clients1.google.com   ...
//
// accounts.google.com is not there, which is why sign-in has to go through the revival host
// today. Four of the entries name Google's own staging sandboxes, which no console will ever
// reach, so one of them is overwritten in place with the name that is missing. The pointer is
// left alone and the list stays the same length; only the characters it points at change.

#include "whitelist.h"

#include <cstring>
#include <whb/log.h>

#include "image.h"
#include "patch.h"
#include "trace.h"
#include "utils/logger.h"

// The entry given up. 46 characters and a terminator, which is room enough for the 19 wanted.
static const char SACRIFICE[] = "youtubei-googleapis-staging.sandbox.google.com";
static const char WANTED[] = "accounts.google.com";

// Where the entry sits in the version this was read from, as linked.
#define KNOWN_LINK 0x1000e36cu

bool whitelist_allow_accounts() {
    uint32_t known = image_data(KNOWN_LINK);
    uint32_t start = image_data(IMAGE_DATA_LINK);
    uint32_t end = image_data(IMAGE_RODATA_END);

    // sizeof covers the terminator, which is what makes a match exact: without it a longer
    // string starting the same way would match.
    char now[sizeof(SACRIFICE)];
    bool read = patch_read(known, now, sizeof(now));
    if (read && memcmp(now, WANTED, sizeof(WANTED)) == 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: accounts.google.com is already on the whitelist");
        return true;
    }
    uint32_t at = read && memcmp(now, SACRIFICE, sizeof(SACRIFICE)) == 0
                      ? known
                      : patch_find(start, end, SACRIFICE, sizeof(SACRIFICE), 1);
    if (!at) {
        // Already done is the other reason the sacrifice is not there, and it is the common one
        // on a second run of the same boot.
        if (patch_find(start, end, WANTED, sizeof(WANTED), 1)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: accounts.google.com is already on the whitelist");
            return true;
        }
        if (read) {
            trace_line("whitelist entry 0x%08x holds '%.*s'", known, (int) strnlen(now, 32), now);
        } else {
            trace_line("whitelist entry 0x%08x has no page behind it", known);
        }
        DEBUG_FUNCTION_LINE("WiiULeanback: no whitelist entry to give up, sign-in stays proxied");
        return false;
    }

    // The whole slot, so nothing of the old name is left past the terminator.
    uint8_t slot[sizeof(SACRIFICE)];
    memset(slot, 0, sizeof(slot));
    memcpy(slot, WANTED, sizeof(WANTED));
    if (!patch_write(at, slot, sizeof(slot))) {
        trace_line("whitelist entry 0x%08x found, the write did not take", at);
        DEBUG_FUNCTION_LINE("WiiULeanback: the whitelist patch at 0x%08x did not take", at);
        return false;
    }
    trace_line("whitelist entry 0x%08x rewritten", at);
    DEBUG_FUNCTION_LINE("WiiULeanback: accounts.google.com whitelisted at 0x%08x", at);
    return true;
}
