// youtubei.googleapis.com answers 404, with no cross origin headers, to any request whose
// User-Agent starts "Mozilla/5.0 (WiiU; Leanback Shell)", which is what this app sends. The page
// cannot set its own User-Agent, and a 404 it is not allowed to read reaches it as a failed
// request, so every InnerTube call the console makes for itself dies on it. googlevideo, ytimg,
// ggpht, accounts and suggestqueries answer the same agent normally.
//
// The app builds the agent from pieces, "Mozilla/5.0 (%s) AppleWebKit/%d.%d ..." with "WiiU; "
// and "Leanback Shell" making up the %s, and each piece is a string of its own, read from one
// place. Turning the semicolon after WiiU into a comma is enough to miss the match, and nothing
// on the page reads the platform out of the agent.

#include "ua.h"

#include <cstring>
#include <whb/log.h>

#include "image.h"
#include "patch.h"
#include "trace.h"
#include "utils/logger.h"

// With the terminator in front as well as behind, so a search cannot land on the tail of some
// longer string that happens to end the same way. The piece itself starts one byte in.
static const char BLOCKED[] = "\0WiiU; ";
static const char ALLOWED[] = "\0WiiU, ";

// Where the piece sits in the version this was read from, as linked.
#define KNOWN_LINK 0x102c39e0u

bool ua_unblock() {
    uint32_t known = image_data(KNOWN_LINK) - 1;
    uint32_t start = image_data(IMAGE_DATA_LINK);
    uint32_t end = image_data(IMAGE_RODATA_END);

    char now[sizeof(BLOCKED)];
    bool read = patch_read(known, now, sizeof(now));
    if (read && memcmp(now, ALLOWED, sizeof(ALLOWED)) == 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: the user agent is already changed");
        return true;
    }
    uint32_t at = read && memcmp(now, BLOCKED, sizeof(BLOCKED)) == 0
                      ? known
                      : patch_find(start, end, BLOCKED, sizeof(BLOCKED), 1);
    if (!at) {
        if (patch_find(start, end, ALLOWED, sizeof(ALLOWED), 1)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: the user agent is already changed");
            return true;
        }
        if (read) {
            trace_line("user agent piece 0x%08x holds '%.*s'", known + 1,
                       (int) strnlen(now + 1, sizeof(now) - 1), now + 1);
        } else {
            trace_line("user agent piece 0x%08x has no page behind it", known + 1);
        }
        return false;
    }
    if (!patch_write(at, ALLOWED, sizeof(ALLOWED))) {
        trace_line("user agent piece 0x%08x found, the write did not take", at + 1);
        return false;
    }
    trace_line("user agent piece 0x%08x rewritten", at + 1);
    DEBUG_FUNCTION_LINE("WiiULeanback: user agent changed at 0x%08x", at + 1);
    return true;
}
