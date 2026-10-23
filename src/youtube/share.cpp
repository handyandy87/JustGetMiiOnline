// The Share request's checks. Nothing here touches a socket, a clock or the console, so the Mac
// test builds it as it is.

#include "share.h"

#include <cstring>
#include <strings.h>

#define MINUTE_MS 60000u

enum { HEADER_NONE, HEADER_FOUND, HEADER_LONG };

// A header's value with the spaces and tabs around it trimmed. A value that doesn't fit isn't
// copied, so a cut one can never pass for a shorter one.
static int header(const char *headers, const char *name, char *out, size_t max) {
    out[0] = 0;
    size_t nameLen = strlen(name);
    const char *at = strstr(headers, "\r\n");
    while (at) {
        at += 2;
        const char *end = strstr(at, "\r\n");
        if (!end || end == at) return HEADER_NONE;
        if ((size_t) (end - at) > nameLen && strncasecmp(at, name, nameLen) == 0 &&
            at[nameLen] == ':') {
            const char *v = at + nameLen + 1;
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            const char *stop = end;
            while (stop > v && (stop[-1] == ' ' || stop[-1] == '\t')) stop--;
            size_t len = (size_t) (stop - v);
            if (len >= max) return HEADER_LONG;
            memcpy(out, v, len);
            out[len] = 0;
            return HEADER_FOUND;
        }
        at = end;
    }
    return HEADER_NONE;
}

// Where the origin's host ends when it starts with https:// and the host is ours, or null.
static const char *after_host(const char *url) {
    static const char SCHEME[] = "https://";
    static const char HOST[] = "www.youtube.com";
    if (strncmp(url, SCHEME, sizeof(SCHEME) - 1) != 0) return nullptr;
    const char *host = url + sizeof(SCHEME) - 1;
    if (strncasecmp(host, HOST, sizeof(HOST) - 1) != 0) return nullptr;
    return host + sizeof(HOST) - 1;
}

bool share_origin_ok(const char *origin, const char *referer) {
    if (origin && origin[0]) {
        const char *end = after_host(origin);
        return end && !*end;
    }
    if (referer && referer[0]) {
        const char *end = after_host(referer);
        return end && (!*end || *end == '/');
    }
    return false;
}

bool share_token_equal(const char *given, size_t length, const char *token) {
    size_t n = token ? strlen(token) : 0;
    if (n < SHARE_TOKEN_MIN || !given) return false;
    // Every byte of the token is looked at whatever the given one holds, so the time taken says
    // how long the token is and nothing about where they part.
    volatile unsigned diff = length != n;
    for (size_t i = 0; i < n; i++) {
        unsigned char g = i < length ? (unsigned char) given[i] : 0;
        diff = diff | (unsigned) (g ^ (unsigned char) token[i]);
    }
    return diff == 0;
}

bool share_text_ok(const char *bytes, size_t length) {
    const unsigned char *s = (const unsigned char *) bytes;
    size_t i = 0;
    while (i < length) {
        unsigned char c = s[i];
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t more;
        unsigned lowest;
        unsigned value;
        if (c >= 0xc2 && c <= 0xdf) {
            more = 1;
            lowest = 0x80;
            value = c & 0x1f;
        } else if (c >= 0xe0 && c <= 0xef) {
            more = 2;
            lowest = 0x800;
            value = c & 0x0f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            more = 3;
            lowest = 0x10000;
            value = c & 0x07;
        } else {
            return false;
        }
        if (length - i <= more) return false;
        for (size_t j = 1; j <= more; j++) {
            if ((s[i + j] & 0xc0) != 0x80) return false;
            value = value << 6 | (s[i + j] & 0x3f);
        }
        // Overlong forms, the surrogates and anything past U+10FFFF.
        if (value < lowest || (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

size_t share_text_cut(const char *bytes, size_t length, size_t max) {
    if (length <= max) return length;
    size_t n = max;
    while (n > 0 && ((unsigned char) bytes[n] & 0xc0) == 0x80) n--;
    return n;
}

bool share_rate_take(ShareRate *rate, uint64_t nowMs) {
    std::lock_guard<std::mutex> hold(rate->lock);
    // The slot about to be reused holds the oldest of the last limit. A clock behind it counts as
    // inside the minute.
    if (rate->used == rate->limit) {
        uint64_t oldest = rate->at[rate->next];
        if (nowMs < oldest || nowMs - oldest < MINUTE_MS) return false;
    } else {
        rate->used++;
    }
    rate->at[rate->next] = nowMs;
    rate->next = (rate->next + 1) % rate->limit;
    return true;
}

// Origin, or Referer without one, out of the headers.
static bool origin_from(const char *headers) {
    char origin[64];
    char referer[256];
    if (header(headers, "Origin", origin, sizeof(origin)) == HEADER_LONG) return false;
    int hasReferer = HEADER_NONE;
    if (!origin[0]) {
        hasReferer = header(headers, "Referer", referer, sizeof(referer));
        if (hasReferer == HEADER_LONG) return false;
    }
    return share_origin_ok(origin, hasReferer == HEADER_FOUND ? referer : nullptr);
}

int share_head(const char *headers, size_t *length) {
    *length = 0;
    if (strncmp(headers, "POST ", 5) != 0) return 405;
    if (!origin_from(headers)) return 403;

    char text[32];
    if (header(headers, "Transfer-Encoding", text, sizeof(text)) != HEADER_NONE) return 400;
    int hasLength = header(headers, "Content-Length", text, sizeof(text));
    if (hasLength == HEADER_LONG) return 413;
    if (hasLength == HEADER_NONE || !text[0]) return 400;
    size_t n = 0;
    for (const char *at = text; *at; at++) {
        if (*at < '0' || *at > '9') return 400;
        n = n * 10 + (size_t) (*at - '0');
        if (n > SHARE_BODY_MAX) return 413;
    }
    *length = n;
    return 0;
}

// A plain memset on a local that is never read again can be dropped by the compiler.
static void wipe(void *at, size_t bytes) {
    volatile unsigned char *p = (volatile unsigned char *) at;
    while (bytes--) *p++ = 0;
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decodes from..to as a form part, '+' for a space and %XX for a byte, onto the end of out.
// Answers how many bytes it added, or -1 for a broken escape.
static long decode(const char *from, const char *to, char *out) {
    long n = 0;
    while (from < to) {
        char c = *from++;
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            int hi = to - from >= 2 ? hex(from[0]) : -1;
            int lo = hi >= 0 ? hex(from[1]) : -1;
            if (lo < 0) return -1;
            c = (char) (hi << 4 | lo);
            from += 2;
        }
        out[n++] = c;
    }
    return n;
}

enum { FIELD_V, FIELD_T, FIELD_C, FIELD_K, FIELDS };
static const char *const NAMES[FIELDS] = {"v", "t", "c", "k"};

struct Span {
    const char *at;
    size_t length;
    bool seen;
};

// Splits the form into the four fields, decoded into out, which needs length bytes. Any other
// field is passed over. A broken escape or a field sent twice is a malformed form.
static bool split_form(const char *body, size_t length, char *out, Span *spans) {
    size_t used = 0;
    const char *end = body + length;
    const char *at = body;
    while (at < end) {
        const char *amp = (const char *) memchr(at, '&', (size_t) (end - at));
        const char *stop = amp ? amp : end;
        const char *eq = (const char *) memchr(at, '=', (size_t) (stop - at));
        const char *nameEnd = eq ? eq : stop;
        long nameLen = decode(at, nameEnd, out + used);
        if (nameLen < 0) return false;
        int which = -1;
        for (int f = 0; f < FIELDS; f++) {
            if (nameLen == 1 && out[used] == NAMES[f][0]) which = f;
        }
        long valueLen = eq ? decode(eq + 1, stop, out + used) : 0;
        if (valueLen < 0) return false;
        if (which >= 0) {
            if (spans[which].seen) return false;
            spans[which].seen = true;
            spans[which].at = out + used;
            spans[which].length = (size_t) valueLen;
            used += (size_t) valueLen;
        }
        at = amp ? amp + 1 : end;
    }
    return true;
}

static bool id_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

static int check_fields(ShareRate *rate, const Span *spans, uint64_t nowMs, ShareFields *out) {
    const Span &k = spans[FIELD_K];
    if (!k.seen || !share_token_equal(k.at, k.length, share_token())) return 403;
    if (!share_rate_take(rate, nowMs)) return 429;

    const Span &v = spans[FIELD_V];
    if (!v.seen || v.length != SHARE_ID_BYTES) return 400;
    for (size_t i = 0; i < SHARE_ID_BYTES; i++) {
        if (!id_char(v.at[i])) return 400;
    }
    const Span &t = spans[FIELD_T];
    if (!t.seen || !t.length || !share_text_ok(t.at, t.length)) return 400;
    const Span &c = spans[FIELD_C];
    if (c.seen && !share_text_ok(c.at, c.length)) return 400;

    memcpy(out->v, v.at, SHARE_ID_BYTES);
    out->v[SHARE_ID_BYTES] = 0;
    size_t tn = share_text_cut(t.at, t.length, SHARE_TITLE_MAX);
    memcpy(out->t, t.at, tn);
    out->t[tn] = 0;
    size_t cn = c.seen ? share_text_cut(c.at, c.length, SHARE_CHANNEL_MAX) : 0;
    if (cn) memcpy(out->c, c.at, cn);
    out->c[cn] = 0;
    return 0;
}

int share_body(ShareRate *rate, const char *body, size_t length, uint64_t nowMs,
               ShareFields *fields) {
    if (length > SHARE_BODY_MAX) return 413;
    // Decoding never makes a field longer, so the body's length is room for all of them.
    char decoded[SHARE_BODY_MAX];
    Span spans[FIELDS] = {};
    ShareFields mine;
    ShareFields *out = fields ? fields : &mine;
    memset(out, 0, sizeof(*out));
    int status = split_form(body, length, decoded, spans) ? 0 : 400;
    if (!status) status = check_fields(rate, spans, nowMs, out);
    wipe(decoded, sizeof(decoded));
    if (status) return status;

    static std::mutex screen;
    std::lock_guard<std::mutex> hold(screen);
    status = proxy_share(out->v, out->t, out->c);
    if (status != 202 && status != 409 && status != 503 && status != 400) status = 503;
    return status;
}

const char *share_state_text(int state) {
    switch (state) {
        case SHARE_STATE_IDLE: return "idle";
        case SHARE_STATE_OPENING: return "opening";
        case SHARE_STATE_OPEN: return "open";
        case SHARE_STATE_FAILED: return "failed";
        default: return nullptr;
    }
}

int share_state(ShareRate *rate, const char *headers, uint64_t nowMs, const char **text) {
    *text = "";
    if (strncmp(headers, "GET ", 4) != 0) return 405;
    if (!origin_from(headers)) return 403;

    // The query is whatever follows the path's '?' up to the space before the version.
    const char *target = headers + 4;
    const char *eol = strstr(target, "\r\n");
    const char *stop = (const char *) memchr(target, ' ', eol ? (size_t) (eol - target) : 0);
    if (!stop) return 400;
    const char *query = (const char *) memchr(target, '?', (size_t) (stop - target));
    query = query ? query + 1 : stop;
    size_t length = (size_t) (stop - query);
    if (length > SHARE_BODY_MAX) return 400;
    char decoded[SHARE_BODY_MAX];
    Span spans[FIELDS] = {};
    int status = split_form(query, length, decoded, spans) ? 0 : 400;
    const Span &k = spans[FIELD_K];
    if (!status && (!k.seen || !share_token_equal(k.at, k.length, share_token()))) status = 403;
    wipe(decoded, sizeof(decoded));
    if (status) return status;
    if (!share_rate_take(rate, nowMs)) return 429;

    const char *word = share_state_text(proxy_share_state());
    if (!word) return 503;
    *text = word;
    return 200;
}

const char *share_status_line(int status) {
    switch (status) {
        case 200: return "200 OK";
        case 202: return "202 Accepted";
        case 400: return "400 Bad Request";
        case 403: return "403 Forbidden";
        case 405: return "405 Method Not Allowed";
        case 409: return "409 Conflict";
        case 413: return "413 Payload Too Large";
        case 429: return "429 Too Many Requests";
        default: return "503 Service Unavailable";
    }
}
