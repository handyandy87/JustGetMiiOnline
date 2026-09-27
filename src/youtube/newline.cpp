// The shell drives the GamePad keyboard in its keyboard only mode and keeps the text in the page.
// Its receiver gets the keyboard's string and the range that changed, and sends the page a key
// for every character, through one helper that posts a raw key down, a char and a key up. OK
// goes through the same helper from the shell's per frame poll. Read out of the title, a typed
// newline is turned into the Enter key on the way:
//
//     cmpwi  r5,0xa            the character is a newline
//     ...
//     lbz    r4,0xd(r12)       the key code, the shell's table entry for 13, which is 13
//     li     r5,0xd            and 13 for the char
//     or.    r0,r4,r5
//
// and OK is posted as 13 and 13 as well. The helper's last argument differs between the two, but
// it only reaches a filter ahead of WebKit, so the page gets the same three events from either
// key and can't tell a new line from done. Making the char 10 for a typed newline keeps the key
// code at 13 for both and gives the page a char to tell them apart by. One instruction.

#include "newline.h"

#include <cstring>
#include <whb/log.h>

#include "image.h"
#include "patch.h"
#include "trace.h"
#include "utils/logger.h"

// lbz r4,0xd(r12) ; li r5,0xd ; or. r0,r4,r5, which appears once in the text
static const uint8_t SIGNATURE[] = {0x88, 0x8c, 0x00, 0x0d,
                                    0x38, 0xa0, 0x00, 0x0d,
                                    0x7c, 0x80, 0x2b, 0x79};

// li r5,0xa in place of the li in the middle
static const uint8_t PATCHED[] = {0x88, 0x8c, 0x00, 0x0d,
                                  0x38, 0xa0, 0x00, 0x0a,
                                  0x7c, 0x80, 0x2b, 0x79};

// Where it sits in the version this was read from, as linked. Tried first, moved with the image,
// and the search covers a title that has moved it.
#define KNOWN_LINK 0x020d1f68u

bool newline_apart() {
    uint32_t known = image_text(KNOWN_LINK);
    uint32_t start = image_text(IMAGE_TEXT_LINK);
    uint32_t end = image_text(IMAGE_TEXT_END);

    uint8_t now[sizeof(SIGNATURE)];
    bool read = patch_read(known, now, sizeof(now));
    if (read && memcmp(now, PATCHED, sizeof(now)) == 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: keyboard newline already apart at 0x%08x", known);
        return true;
    }
    uint32_t at = read && memcmp(now, SIGNATURE, sizeof(now)) == 0
                      ? known
                      : patch_find(start, end, SIGNATURE, sizeof(SIGNATURE), 4);
    if (!at) {
        if (patch_find(start, end, PATCHED, sizeof(PATCHED), 4)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: keyboard newline already apart");
            return true;
        }
        if (read) {
            trace_line("newline site 0x%08x holds %02x%02x%02x%02x %02x%02x%02x%02x", known,
                       now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
        } else {
            trace_line("newline site 0x%08x has no page behind it", known);
        }
        DEBUG_FUNCTION_LINE("WiiULeanback: no newline site to patch, Return stays the same as OK");
        return false;
    }
    // Only the middle instruction changes. The two either side are written back as they were, so
    // the read-back covers the whole signature and a half applied write cannot pass.
    if (!patch_write(at, PATCHED, sizeof(PATCHED))) {
        trace_line("newline site 0x%08x found, the write did not take", at);
        DEBUG_FUNCTION_LINE("WiiULeanback: the newline patch at 0x%08x did not take", at);
        return false;
    }
    trace_line("newline site 0x%08x patched", at);
    DEBUG_FUNCTION_LINE("WiiULeanback: keyboard newline apart from OK, patched 0x%08x", at);
    return true;
}
