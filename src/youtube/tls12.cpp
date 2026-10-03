// The app tops out at TLS 1.1 on the wire. Google still answers that, but 1.1 is long deprecated,
// and it is a setting rather than a limit: the OpenSSL the app ships is 1.0.1, built with TLS 1.2
// and the GCM suites, and the clamp comes from Chromium's net layer in
// ssl_client_socket_openssl.cc, which sets SSL_OP_NO_TLSv1_2 whenever its version_max is below
// TLS 1.2.
//
// Read out of the title, the block builds that flag like this:
//
//     lhz    r9,0xc2(r30)     the configured version_max
//     cmplwi r9,0x0303        TLS1_2_VERSION
//     li     r0,1
//     xori   r5,r0,1          the flag: set NO_TLSv1_2 when max is below 1.2
//     addi   r3,r1,8
//     lis    r4,0x0800        SSL_OP_NO_TLSv1_2
//     bl     ConfigureFlag
//
// Turning the xori into li r5,0 configures the flag off every time, which puts it in the clear
// mask rather than the set mask, and the hello then offers 1.2. One instruction, and nothing
// about which versions are compiled in changes.

#include "tls12.h"

#include <cstring>
#include <whb/log.h>

#include "image.h"
#include "patch.h"
#include "trace.h"
#include "utils/logger.h"

// xori r5,r0,1 ; addi r3,r1,8 ; lis r4,0x0800. Three instructions rather than one, because the
// xori on its own appears sixteen times in the text and this appears once.
static const uint8_t SIGNATURE[] = {0x68, 0x05, 0x00, 0x01,
                                    0x38, 0x61, 0x00, 0x08,
                                    0x3c, 0x80, 0x08, 0x00};

// li r5,0, which replaces the xori at the front of the signature
static const uint8_t PATCHED[] = {0x38, 0xa0, 0x00, 0x00,
                                  0x38, 0x61, 0x00, 0x08,
                                  0x3c, 0x80, 0x08, 0x00};

// Where it sits in the version this was read from, as linked. Tried first, moved with the image,
// so the usual case costs one read, and the search covers a title that has moved it.
#define KNOWN_LINK 0x02722420u

bool tls12_enable() {
    uint32_t known = image_text(KNOWN_LINK);
    uint32_t start = image_text(IMAGE_TEXT_LINK);
    uint32_t end = image_text(IMAGE_TEXT_END);

    uint8_t now[sizeof(SIGNATURE)];
    bool read = patch_read(known, now, sizeof(now));
    if (read && memcmp(now, PATCHED, sizeof(now)) == 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: tls 1.2 already on at 0x%08x", known);
        return true;
    }
    uint32_t at = read && memcmp(now, SIGNATURE, sizeof(now)) == 0
                      ? known
                      : patch_find(start, end, SIGNATURE, sizeof(SIGNATURE), 4);
    if (!at) {
        if (patch_find(start, end, PATCHED, sizeof(PATCHED), 4)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: tls 1.2 already on");
            return true;
        }
        if (read) {
            trace_line("tls site 0x%08x holds %02x%02x%02x%02x %02x%02x%02x%02x", known, now[0],
                       now[1], now[2], now[3], now[4], now[5], now[6], now[7]);
        } else {
            trace_line("tls site 0x%08x has no page behind it", known);
        }
        DEBUG_FUNCTION_LINE("WiiULeanback: no tls site to patch, the app stays on TLS 1.1");
        return false;
    }
    // Only the first instruction changes. The two after it are written back as they were, so the
    // read-back covers the whole signature and a half applied write cannot pass.
    if (!patch_write(at, PATCHED, sizeof(PATCHED))) {
        trace_line("tls site 0x%08x found, the write did not take", at);
        DEBUG_FUNCTION_LINE("WiiULeanback: the tls patch at 0x%08x did not take", at);
        return false;
    }
    trace_line("tls site 0x%08x patched", at);
    DEBUG_FUNCTION_LINE("WiiULeanback: tls 1.2 on, patched 0x%08x", at);
    return true;
}
