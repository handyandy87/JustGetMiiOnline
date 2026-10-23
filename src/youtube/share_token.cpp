// The Share token, made at every start. It only has to keep another page on the console from
// guessing it. It's 128 bits, and every guess costs a connection to the listener.
//
// Cafe OS hands an app no random source: wut has no call for one, newlib's getentropy has no
// syscall behind it, and nothing in wut reaches IOSU's own generator. So this times short
// loops of scattered memory writes against the time base. Cache misses, interrupts and the other
// cores' bus traffic make the durations jitter by a few ticks, and those durations, with the boot
// time and the wall clock, go through splitmix64's finalizer into two 64 bit lanes. 2,048
// durations go in, 1,024 to a lane, which is far more than the 64 bits each lane can hold even at
// a fraction of a bit apiece. A run whose durations never change gets no token, and every request
// then fails on it.

#include "share.h"

#include <coreinit/core.h>
#include <coreinit/time.h>
#include <cstdlib>
#include <cstring>

#define SAMPLES 2048
// Twice the L1 data cache, so the writes miss it now and then.
#define SCRATCH_BYTES (64 * 1024)

static char sToken[SHARE_TOKEN_MAX + 1];
static bool sMade = false;

static uint64_t mix(uint64_t state, uint64_t in) {
    state ^= in;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ull;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebull;
    return state ^ (state >> 31);
}

static uint32_t timed(volatile uint8_t *scratch, uint32_t mask, uint32_t seed) {
    OSTick start = OSGetSystemTick();
    uint32_t index = seed;
    for (int i = 0; i < 64; i++) {
        index = index * 1664525u + 1013904223u;
        scratch[(index >> 8) & mask] += (uint8_t) index;
    }
    return (uint32_t) (OSGetSystemTick() - start);
}

// A plain memset on a local that is never read again can be dropped by the compiler.
static void wipe(void *at, size_t bytes) {
    volatile uint8_t *p = (volatile uint8_t *) at;
    while (bytes--) *p++ = 0;
}

void share_token_new() {
    sMade = true;
    wipe(sToken, sizeof(sToken));
    // The stack fallback is small enough to stay in the cache, which leaves less jitter but not
    // none, and the check below still stands.
    uint8_t small[2048];
    uint8_t *heap = (uint8_t *) malloc(SCRATCH_BYTES);
    volatile uint8_t *scratch = heap ? heap : small;
    uint32_t mask = (heap ? SCRATCH_BYTES : sizeof(small)) - 1;

    uint64_t lanes[2] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull};
    lanes[0] = mix(lanes[0], (uint64_t) OSGetSystemTime());
    lanes[1] = mix(lanes[1], (uint64_t) OSGetTime() ^ ((uint64_t) OSGetCoreId() << 32));
    uint32_t first = 0;
    bool varied = false;
    for (uint32_t i = 0; i < SAMPLES; i++) {
        uint64_t &lane = lanes[i & 1];
        uint32_t d = timed(scratch, mask, (uint32_t) lane + i);
        if (i == 0) first = d;
        varied = varied || d != first;
        lane = mix(lane, d);
    }
    lanes[0] = mix(lanes[0], (uint64_t) OSGetSystemTime() ^ lanes[1]);
    lanes[1] = mix(lanes[1], lanes[0]);
    free(heap);
    wipe(small, sizeof(small));

    if (varied) {
        static const char DIGITS[] = "0123456789abcdef";
        for (int i = 0; i < SHARE_TOKEN_MAX; i++) {
            uint64_t lane = lanes[i / 16];
            sToken[i] = DIGITS[(lane >> ((i % 16) * 4)) & 0xf];
        }
        sToken[SHARE_TOKEN_MAX] = 0;
    }
    wipe(lanes, sizeof(lanes));
}

const char *share_token() {
    if (!sMade) share_token_new();
    return sToken;
}
