#include "trace.h"

#include "settings.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

// Beside the plugin's config folder rather than inside it. That folder can be mounted over
// /vol/content as a layer, so anything put in it turns up among the app's own files.
#define TRACE_DIR    "fs:/vol/external01/wiiu/environments/aroma/plugins/config"
#define TRACE_OUT    TRACE_DIR "/WiiULeanback.trace.txt"

// Enough for a boot and a browse. Past it lines are counted and dropped rather than wrapping,
// since the interesting part is the beginning.
#define TRACE_BYTES (192 * 1024)
#define TRACE_LINE  240

// Taken once and kept. Handing it back at the end of a run would race the threads still in
// trace_line, and a run only reaches here with tracing on anyway.
static char *sOwned = nullptr;
static std::atomic<char *> sBuf{nullptr};
static std::atomic<uint32_t> sUsed{0};
static std::atomic<uint32_t> sDropped{0};
static std::atomic<bool> sBusy{false};

bool trace_on() {
    return sBuf.load(std::memory_order_relaxed) != nullptr;
}

bool trace_busy() {
    return sBusy.load(std::memory_order_relaxed);
}

void trace_start() {
    if (!settings_trace()) {
        return;
    }
    if (!sOwned) {
        sOwned = (char *) malloc(TRACE_BYTES);
    }
    if (!sOwned) {
        return;
    }
    sUsed = 0;
    sDropped = 0;
    sBuf.store(sOwned, std::memory_order_release);
}

void trace_line(const char *fmt, ...) {
    char *buf = sBuf.load(std::memory_order_acquire);
    if (!buf) {
        return;
    }
    char line[TRACE_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int) sizeof(line) - 2) {
        n = sizeof(line) - 2;
    }
    line[n++] = '\n';

    // Every thread the shell runs writes here, so the space is claimed before the copy. Claiming
    // nothing on overflow keeps the used count honest for whoever writes the file out.
    uint32_t at = sUsed.load(std::memory_order_relaxed);
    for (;;) {
        if (at + (uint32_t) n > TRACE_BYTES) {
            sDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (sUsed.compare_exchange_weak(at, at + (uint32_t) n)) {
            break;
        }
    }
    memcpy(buf + at, line, n);
}

// The app's shell prints its own requests, and a signed in viewer's token rides in them. A line
// is cut at the first of these rather than filtered, since whatever follows one is the secret.
static const char *const kSecrets[] = {"access_token", "refresh_token", "Authorization", "Bearer "};

void trace_text(const char *tag, const char *msg, uint32_t size) {
    if (!msg || !size || !trace_on() || trace_busy()) {
        return;
    }
    while (size && (msg[size - 1] == '\n' || msg[size - 1] == '\r')) {
        size--;
    }
    if (size > TRACE_LINE) {
        size = TRACE_LINE;
    }
    for (uint32_t i = 0; i < size; i++) {
        for (const char *secret : kSecrets) {
            size_t n = strlen(secret);
            if (i + n <= size && memcmp(msg + i, secret, n) == 0) {
                trace_line("%s %.*s<cut>", tag, (int) i, msg);
                return;
            }
        }
    }
    if (size) {
        trace_line("%s %.*s", tag, (int) size, msg);
    }
}

void trace_end() {
    char *buf = sBuf.exchange(nullptr, std::memory_order_acq_rel);
    if (!buf) {
        return;
    }
    sBusy = true;
    FILE *f = fopen(TRACE_OUT, "ab");
    if (f) {
        fwrite(buf, 1, sUsed.load(), f);
        uint32_t dropped = sDropped.load();
        if (dropped) {
            fprintf(f, "%u more lines, buffer full\n", dropped);
        }
        fputs("--- end of run ---\n", f);
        fclose(f);
    }
    sBusy = false;
}
