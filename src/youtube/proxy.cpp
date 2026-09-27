// Second screen pairing is the one thing left that the page cannot ask Google for itself. The
// lounge api carries no Access-Control-Allow-Origin at all, not even for youtube.com, so nothing
// in a browser can read its answer from another origin. Google will not change that, so
// something on the console has to answer instead, and this is it.
//
// The page asks lb-proxy.youtube.com, which getaddrinfo answers with the console itself. This
// takes the request, carries it to www.youtube.com over TLS, and hands the answer back with the
// headers the page needs added. It is the same proxy the revival host was running, moved onto the
// console, which is the honest description of what it does. Only /api/lounge/ is carried.
//
// Plain http on the listening side, read by a page served over https. The shell lets that page
// read a plain http answer only when its type starts with text/plain, text/xml, image/, video/,
// audio/, application/octet-stream or binary/octet-stream, or when it is a 204 with no body.
// Anything else reaches the page as status 0. So every answer here goes out as one of those, and
// json goes out as text/plain with "json" kept in a parameter, since the app only parses an
// answer whose type has "json" in it. A listener that spoke https would need a private key
// sitting on the card, where it protects nothing.
//
// The back channel is a long poll: youtube holds it open and writes down it when the phone sends
// something. So the body is relayed as it arrives rather than read to the end first, which is
// what made the host's own version work. There is no receive timeout on this platform, so a
// connection sits on its read until the other side closes, which is what the app does when the
// viewer leaves the screen. Every live connection's sockets are kept in a table so stopping can
// shut them down and break whatever read a worker is sitting on.

#include "proxy.h"

#include "settings.h"
#include "trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <arpa/inet.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <nsysnet/nssl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <whb/log.h>

#include "utils/logger.h"

#define UPSTREAM_HOST "www.youtube.com"
#define UPSTREAM_PORT 443

// A pairing run needs the four rest calls, and the channel after it holds a forward and a back
// connection open at once. Eight leaves room above the four or five that implies. The cap only
// bites if connections stop being returned, which the deadlines below are what prevent.
#define MAX_CONNECTIONS 8

// How long a read waits before the connection is given up on. The app's own side is quick, so a
// request that stalls half way through is not coming. The back channel is meant to sit idle, so
// its deadline is long and is there to break a wedge rather than to time anything.
#define CLIENT_IDLE_SECONDS 30
#define UPSTREAM_IDLE_SECONDS 120

// How long a stop waits for the workers to fall out of their reads before giving up on them.
#define DRAIN_SECONDS 5
// What a worker gets. The frame comes to a little under 27 KB: the request, which the rewritten
// answer reuses, and a relay chunk's worth the other buffers share, each in a block of its own so
// the compiler can overlap them. Nothing below it allocates.
#define WORKER_STACK (64 * 1024)
// What the accept thread gets, a fraction of the 128 KB a thread gets by default. It only
// accepts, claims a slot and starts a worker.
#define ACCEPT_STACK (16 * 1024)
// Headers have to arrive whole before anything can be decided, and 8 KB is far more than the
// pairing calls send.
#define HEADER_MAX 8192
// The fixed part of the cross origin block is a little over 300 bytes, and the origin echoed
// into it is bounded by the buffer it was read into.
#define ORIGIN_MAX 256
#define CORS_MAX (ORIGIN_MAX + 384)
// The one path carried upstream, and the part of it the lounge's channel sits under.
#define LOUNGE_PATH "/api/lounge/"
#define CHANNEL_PATH "/api/lounge/bc/"
// What a json answer goes out as. The shell reads the type up to its parameters, and the app's
// own check is for "json" anywhere in the header.
#define JSON_TYPE "text/plain; charset=utf-8; format=json"
// A TLS record holds up to 16 KB, and NSSL decrypts a whole one before handing any of it back.
// A chunk smaller than a record means the rest sits inside NSSL where select cannot see it, and
// the relay below has to guess about it, so the chunk is a record.
#define RELAY_CHUNK 16384

static std::atomic<bool> sRunning{false};
static std::atomic<int> sListen{-1};
static pthread_t sAccept;
static bool sAccepting = false;
static NSSLContextHandle sTls = -1;

// Whether the listener actually came up, which is not the same as whether it was asked for. The
// name only gets answered locally when this is true, so a start that failed leaves the lookup
// alone and the page falls back instead of meeting a refused connection.
static std::atomic<bool> sUp{false};
bool proxy_running() { return sUp.load(); }

// One shared context, and creating a connection against it from several threads at once is not
// something the library says anything about. Serialized, since setup is not the hot path.
static std::mutex sNsslLock;

// Every live connection's two sockets. A worker parked in a read does not see the flag above, so
// stopping breaks the sockets under it instead. Closing happens under the same lock as the
// shutdown, so a shutdown can never land on a descriptor that has been handed to someone else.
struct Live {
    int client;
    int up;
};
static std::mutex sLiveLock;
static Live sLive[MAX_CONNECTIONS];
static bool sLiveReady = false;

static int live_claim(int fd) {
    std::lock_guard<std::mutex> hold(sLiveLock);
    if (!sLiveReady) {
        for (int i = 0; i < MAX_CONNECTIONS; i++) { sLive[i].client = -1; sLive[i].up = -1; }
        sLiveReady = true;
    }
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (sLive[i].client < 0) { sLive[i].client = fd; sLive[i].up = -1; return i; }
    }
    return -1;
}

static void live_upstream(int slot, int fd) {
    std::lock_guard<std::mutex> hold(sLiveLock);
    sLive[slot].up = fd;
}

static void live_close(int slot) {
    std::lock_guard<std::mutex> hold(sLiveLock);
    if (sLive[slot].up >= 0) close(sLive[slot].up);
    if (sLive[slot].client >= 0) close(sLive[slot].client);
    sLive[slot].client = -1;
    sLive[slot].up = -1;
}

static int live_count() {
    std::lock_guard<std::mutex> hold(sLiveLock);
    int n = 0;
    for (int i = 0; i < MAX_CONNECTIONS; i++) { if (sLive[i].client >= 0) n++; }
    return n;
}

static void live_break_all() {
    std::lock_guard<std::mutex> hold(sLiveLock);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (sLive[i].up >= 0) shutdown(sLive[i].up, SHUT_RDWR);
        if (sLive[i].client >= 0) shutdown(sLive[i].client, SHUT_RDWR);
    }
}

static bool upstream_address(struct sockaddr_in *out) {
    struct addrinfo hints{};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (proxy_resolve(UPSTREAM_HOST, "443", &hints, &res) != 0 || !res) {
        return false;
    }
    memcpy(out, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);
    return true;
}

// Whether there is something to read before the deadline. wut has no SO_RCVTIMEO, so without
// this a read sits forever and the connection is never handed back, which turns the cap above
// into a countdown rather than a limit.
//
// On the upstream side this watches the socket under the TLS, so it can say nothing is coming
// while NSSL still holds a decrypted record. A read that fills the buffer is taken as a sign
// there is more and skips the wait, which covers the case that matters.
//
// FD_SETSIZE is 32 here and an fd_set is one word, so a descriptor at or above it would be set
// past the end of a stack object. The whole process shares one descriptor table, so that number
// is not this file's to predict.
static bool waitable(int fd, int seconds, bool forWriting) {
    if (fd < 0 || fd >= FD_SETSIZE) return false;
    fd_set set;
    struct timeval wait;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    wait.tv_sec = seconds;
    wait.tv_usec = 0;
    fd_set *read = forWriting ? nullptr : &set;
    fd_set *write = forWriting ? &set : nullptr;
    return select(fd + 1, read, write, nullptr, &wait) > 0;
}

static bool readable(int fd, int seconds) { return waitable(fd, seconds, false); }

// A page that stopped reading leaves the send buffer full and the send itself sitting there, so
// writing needs the same deadline reading has.
static bool send_all(int fd, const char *data, int length) {
    int sent = 0;
    while (sent < length) {
        if (!waitable(fd, CLIENT_IDLE_SECONDS, true)) return false;
        int n = send(fd, data + sent, length - sent, 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static bool tls_write_all(NSSLConnectionHandle c, const char *data, int length) {
    int sent = 0;
    while (sent < length) {
        int32_t n = 0;
        if (NSSLWrite(c, data + sent, length - sent, &n) < 0 || n <= 0) return false;
        sent += n;
    }
    return true;
}

// Where the blank line that ends a header block finishes, looking at what the last read added
// to buf and the three bytes before it, or 0 when it has not arrived yet.
static int blank_line_end(const char *buf, int from, int used) {
    for (int i = from > 3 ? from - 3 : 0; i + 4 <= used; i++) {
        if (memcmp(buf + i, "\r\n\r\n", 4) == 0) return i + 4;
    }
    return 0;
}

// Ends the headers with a NUL and moves whatever came after them one byte along, so a lookup
// stops at the headers and the body keeps every byte. That takes one byte past what was read,
// which the readers keep free by reading at most max - 1.
static int split_headers(char *buf, int end, int used, int *extra) {
    memmove(buf + end + 1, buf + end, used - end);
    buf[end] = 0;
    *extra = used - end;
    return end;
}

// Everything up to the blank line, which is where a request or an answer stops being headers.
// Reads whatever has arrived, so body bytes can come in with the headers. Those sit just past
// the NUL, *extra says how many, and the caller sends them on ahead of the rest of the body.
// Answers the headers' length, 0 when the peer closed with nothing, or -1 on a failure. Filling
// the buffer without reaching the blank line is a failure and not a short request: carrying that
// on sends half a header block and treats the rest of the headers as a body.
static int read_headers(int fd, char *buf, int max, int *extra) {
    int used = 0;
    *extra = 0;
    while (used < max - 1) {
        if (!readable(fd, CLIENT_IDLE_SECONDS)) return -1;
        int n = recv(fd, buf + used, max - 1 - used, 0);
        if (n <= 0) return used ? -1 : 0;
        int end = blank_line_end(buf, used, used + n);
        used += n;
        if (end) return split_headers(buf, end, used, extra);
    }
    return -1;
}

// The same for the answer. A read that comes back short has emptied NSSL, so the socket is the
// place to wait on next. One that fills what it asked for may have left the rest of a record
// inside NSSL, and *more hands that on to the relay.
static int tls_read_headers(NSSLConnectionHandle c, int sock, char *buf, int max, int *extra,
                            bool *more) {
    int used = 0;
    *extra = 0;
    while (used < max - 1) {
        if (!readable(sock, UPSTREAM_IDLE_SECONDS)) return -1;
        int32_t want = max - 1 - used;
        int32_t n = 0;
        if (NSSLRead(c, buf + used, want, &n) < 0 || n <= 0) return -1;
        int end = blank_line_end(buf, used, used + n);
        used += n;
        if (end) {
            *more = n == want;
            return split_headers(buf, end, used, extra);
        }
    }
    return -1;
}

// A header's value, or an empty string. Case insensitive on the name, since the app and youtube
// do not agree on how to spell them.
static void header_value(const char *headers, const char *name, char *out, int max) {
    out[0] = 0;
    int nameLen = (int) strlen(name);
    const char *at = headers;
    while (at && *at) {
        const char *line = at;
        const char *end = strstr(line, "\r\n");
        if (!end) break;
        if (strncasecmp(line, name, nameLen) == 0 && line[nameLen] == ':') {
            const char *v = line + nameLen + 1;
            while (*v == ' ') v++;
            int len = (int) (end - v);
            if (len >= max) len = max - 1;
            if (len > 0) memcpy(out, v, len);
            out[len > 0 ? len : 0] = 0;
            return;
        }
        at = end + 2;
    }
}

// What the lounge api never sends and the page cannot do without. The origin is echoed rather
// than fixed, so this does not have to know what the app is served as.
// Answers how much was written, or 0 when it would not all fit. A truncated set of these reaches
// the page as a malformed answer it cannot read, which is the same as not answering, so the
// callers treat not fitting as a failure rather than sending what there was room for.
static int cors_headers(char *out, int max, const char *origin) {
    int n = snprintf(out, max,
                    "Access-Control-Allow-Origin: %s\r\n"
                    "Access-Control-Allow-Credentials: true\r\n"
                    "Access-Control-Allow-Methods: GET,POST,OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Authorization,Content-Type,"
                    "X-YouTube-LoungeId-Token,X-YouTube-Page-CL,X-YouTube-Page-Timestamp\r\n"
                    "Access-Control-Expose-Headers: Content-Type,Content-Length\r\n",
                    origin[0] ? origin : "*");
    return (n > 0 && n < max) ? n : 0;
}

// The type starts the shell lets a page served over https read off a plain http answer.
static const char *const READABLE_TYPES[] = {
    "text/plain", "text/xml", "image/", "video/", "audio/",
    "application/octet-stream", "binary/octet-stream",
};

// What the page is handed in place of the type youtube sent. An answer that says json in any
// form goes out as JSON_TYPE, one the shell already reads keeps its own, and anything else,
// no type at all included, goes out as plain text.
static const char *readable_type(const char *type) {
    for (const char *at = type; *at; at++) {
        if (strncasecmp(at, "json", 4) == 0) return JSON_TYPE;
    }
    for (const char *prefix : READABLE_TYPES) {
        if (strncasecmp(type, prefix, strlen(prefix)) == 0) return type;
    }
    return "text/plain";
}

// A preflight is answered here rather than carried over. youtube answers one 405, which is what
// stops the page in the first place. A 204 with no body passes the shell whatever its type.
static void answer_preflight(int fd, const char *origin) {
    char cors[CORS_MAX];
    if (!cors_headers(cors, sizeof(cors), origin)) return;
    char head[CORS_MAX + 256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 204 No Content\r\n%sAccess-Control-Max-Age: 600\r\n"
                     "Content-Type: text/plain\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     cors);
    if (n > 0 && n < (int) sizeof(head)) send_all(fd, head, n);
}

// No origin has been read at this point, so this names none and leaves credentials off: a
// wildcard and credentials together is a combination a browser refuses.
static void answer_busy(int fd) {
    const char *reply =
        "HTTP/1.1 503 Service Unavailable\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Content-Type: text/plain\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send_all(fd, reply, (int) strlen(reply));
}

// The settings the page needs to know, and that the forwarding is there. Only the 2013 build
// takes an arrangement of the screens and the suggestion strip, so a version whose screens are
// its own is told neither.
static void answer_config(int fd, const char *origin) {
    char body[192];
    const char *screens = settings_screens_name();
    int n = screens ? snprintf(body, sizeof(body),
                               "{\"proxy\":true,\"screens\":\"%s\",\"suggestions\":%s,"
                               "\"comments\":%s}",
                               screens, settings_suggestions() ? "true" : "false",
                               settings_comments() ? "true" : "false")
                    : snprintf(body, sizeof(body),
                               "{\"proxy\":true,\"suggestions\":false,\"comments\":%s}",
                               settings_comments() ? "true" : "false");
    if (n < 0 || n >= (int) sizeof(body)) return;
    char cors[CORS_MAX];
    if (!cors_headers(cors, sizeof(cors), origin)) return;
    char head[CORS_MAX + 256];
    int h = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n%sContent-Type: " JSON_TYPE "\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", cors, n);
    if (h < 0 || h >= (int) sizeof(head)) return;
    if (send_all(fd, head, h)) send_all(fd, body, n);
}

static void answer_plain(int fd, const char *status, const char *origin) {
    char cors[CORS_MAX];
    if (!cors_headers(cors, sizeof(cors), origin)) return;
    char head[CORS_MAX + 256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\n%sContent-Type: text/plain\r\nContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n", status, cors);
    if (n > 0 && n < (int) sizeof(head)) send_all(fd, head, n);
}

// The answer's own headers with ours added. Its status line is kept as it stands, anything it
// already said about cross origin is dropped rather than sent twice, and its type is swapped for
// one the shell lets the page read.
// Answers 0 when any part of it would not fit. Dropping one header and keeping the next hands
// the page an answer missing its Content-Length while it still has a Content-Type, and stopping
// before the blank line makes the body read as more headers. Both are worse than a 502.
static int rewrite_response(const char *from, char *out, int max, const char *origin) {
    const char *end = strstr(from, "\r\n");
    if (!end) return 0;
    int used = snprintf(out, max, "%.*s\r\n", (int) (end - from), from);
    if (used < 0 || used >= max) return 0;
    int cors = cors_headers(out + used, max - used, origin);
    if (!cors) return 0;
    used += cors;
    char type[128];
    header_value(end + 2, "Content-Type", type, sizeof(type));
    int typed = snprintf(out + used, max - used, "Content-Type: %s\r\n", readable_type(type));
    if (typed < 0 || typed >= max - used) return 0;
    used += typed;

    const char *at = end + 2;
    while (at && *at) {
        const char *line = at;
        const char *stop = strstr(line, "\r\n");
        if (!stop || stop == line) break;
        if (strncasecmp(line, "Access-Control-", 15) != 0 &&
            strncasecmp(line, "Content-Type:", 13) != 0) {
            int len = (int) (stop - line);
            if (used + len + 2 > max - 2) return 0;
            memcpy(out + used, line, len);
            used += len;
            out[used++] = '\r';
            out[used++] = '\n';
        }
        at = stop + 2;
    }
    if (used + 2 > max) return 0;
    out[used++] = '\r';
    out[used++] = '\n';
    return used;
}

// Where the request line's target starts and how long it is, or false for a line without one.
static bool request_target(const char *request, const char **target, int *length) {
    const char *eol = strstr(request, "\r\n");
    const char *start = strchr(request, ' ');
    if (!eol || !start || start > eol) return false;
    start++;
    const char *stop = start;
    while (stop < eol && *stop != ' ') stop++;
    if (stop == eol) return false;
    *target = start;
    *length = (int) (stop - start);
    return true;
}

// Whether the target is a lounge call. Past the prefix the path takes lowercase letters, '_' and
// '/' only, which covers every lounge path and leaves no way to walk back out of it.
static bool lounge_target(const char *target, int length) {
    int prefix = (int) strlen(LOUNGE_PATH);
    if (length < prefix || strncmp(target, LOUNGE_PATH, prefix) != 0) return false;
    for (int i = prefix; i < length && target[i] != '?'; i++) {
        char c = target[i];
        if (!(c >= 'a' && c <= 'z') && c != '_' && c != '/') return false;
    }
    return true;
}

// Whether a channel call needs theme=cl added. The app sends none, and the lounge takes a
// channel without one and then sends forceDisconnect unmatchingTheme down it.
static bool needs_theme(const char *target, int length) {
    int prefix = (int) strlen(CHANNEL_PATH);
    if (length < prefix || strncmp(target, CHANNEL_PATH, prefix) != 0) return false;
    const char *query = (const char *) memchr(target, '?', length);
    if (!query) return true;
    for (const char *at = query; at + 7 <= target + length; at++) {
        if ((*at == '?' || *at == '&') && strncmp(at + 1, "theme=", 6) == 0) return false;
    }
    return true;
}

static void serve(int slot) {
    int fd;
    {
        std::lock_guard<std::mutex> hold(sLiveLock);
        fd = sLive[slot].client;
    }
    // Room for the rewritten answer as well, which is built in here once the request has gone.
    char request[HEADER_MAX + 1024];
    char origin[ORIGIN_MAX] = {0};
    NSSLConnectionHandle tls = -1;
    int up = -1;
    int extra = 0;
    const char *target = nullptr;
    int targetLength = 0;

    int got = read_headers(fd, request, HEADER_MAX, &extra);
    if (got < 0) {
        answer_plain(fd, "431 Request Header Fields Too Large", origin);
        goto done;
    }
    if (got == 0) goto done;
    header_value(request, "Origin", origin, sizeof(origin));

    if (strncmp(request, "OPTIONS ", 8) == 0) {
        answer_preflight(fd, origin);
        goto done;
    }
    // The page asks this before it decides what to do, so it is answered here rather than
    // carried anywhere. Nothing else on youtube.com has this path.
    if (strncmp(request, "GET " PROXY_PROBE_PATH " ", 5 + sizeof(PROXY_PROBE_PATH) - 1) == 0) {
        answer_config(fd, origin);
        goto done;
    }
    // Only the lounge is carried. Anything else the page wants goes to whoever served it.
    if (!request_target(request, &target, &targetLength) || !lounge_target(target, targetLength)) {
        answer_plain(fd, "404 Not Found", origin);
        goto done;
    }

    {
        struct sockaddr_in addr{};
        if (!upstream_address(&addr)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: proxy cannot resolve " UPSTREAM_HOST);
            answer_plain(fd, "502 Bad Gateway", origin);
            goto done;
        }
        up = socket(AF_INET, SOCK_STREAM, 0);
        if (up < 0) { answer_plain(fd, "502 Bad Gateway", origin); goto done; }
        live_upstream(slot, up);
        if (connect(up, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
            answer_plain(fd, "502 Bad Gateway", origin);
            goto done;
        }
        {
            std::lock_guard<std::mutex> hold(sNsslLock);
            tls = NSSLCreateConnection(sTls, UPSTREAM_HOST, (int32_t) strlen(UPSTREAM_HOST),
                                       0, up, 1);
        }
        if (tls < 0) {
            DEBUG_FUNCTION_LINE("WiiULeanback: proxy tls refused, %d", (int) tls);
            answer_plain(fd, "502 Bad Gateway", origin);
            goto done;
        }

        // The request goes on as it came, with the host rewritten to the one it is really for.
        // Connection: close so the answer ends at the socket closing, which is what the relay
        // below reads as the end of it.
        {
            char head[HEADER_MAX];
            // What the three headers added below come to, kept free so a request that filled the
            // buffer loses one of its own headers rather than the ones that make it work.
            const int TAIL = 96;
            // The request line first, with theme=cl on the end of a channel call that has none.
            const char *targetEnd = target + targetLength;
            const char *lineEnd = strstr(request, "\r\n");
            const char *theme = !needs_theme(target, targetLength) ? "" :
                                memchr(target, '?', targetLength) ? "&theme=cl" : "?theme=cl";
            int used = snprintf(head, sizeof(head) - TAIL, "%.*s%s%.*s\r\n",
                                (int) (targetEnd - request), request, theme,
                                (int) (lineEnd - targetEnd), targetEnd);
            if (used < 0 || used >= (int) sizeof(head) - TAIL) {
                answer_plain(fd, "431 Request Header Fields Too Large", origin);
                goto done;
            }
            const char *at = lineEnd + 2;
            while (at && *at && used < (int) sizeof(head) - TAIL) {
                const char *stop = strstr(at, "\r\n");
                if (!stop) break;
                int len = (int) (stop - at);
                bool drop = strncasecmp(at, "Host:", 5) == 0 ||
                            strncasecmp(at, "Connection:", 11) == 0 ||
                            strncasecmp(at, "Accept-Encoding:", 16) == 0 ||
                            strncasecmp(at, "Origin:", 7) == 0;
                if (!drop && len > 0 && used + len + 2 < (int) sizeof(head) - TAIL) {
                    memcpy(head + used, at, len);
                    used += len;
                    head[used++] = '\r';
                    head[used++] = '\n';
                }
                if (len == 0) break;
                at = stop + 2;
            }
            // snprintf answers what it wanted to write, not what it wrote, so folding it in
            // without looking walks the write off the end of the buffer.
            int tail = snprintf(head + used, sizeof(head) - used,
                                "Host: " UPSTREAM_HOST "\r\nConnection: close\r\n"
                                "Accept-Encoding: identity\r\n\r\n");
            if (tail < 0 || tail >= (int) sizeof(head) - used) {
                answer_plain(fd, "431 Request Header Fields Too Large", origin);
                goto done;
            }
            used += tail;
            if (!tls_write_all(tls, head, used)) {
                answer_plain(fd, "502 Bad Gateway", origin);
                goto done;
            }
        }

        // Whatever the app sent after its headers. Content-Length rather than chunked, since
        // that is all the app ever sends.
        char lengthText[32];
        header_value(request, "Content-Length", lengthText, sizeof(lengthText));
        // strtol rather than atoi, which has nothing to say about a number too big to hold. The
        // app's bodies are a few hundred bytes, so anything past a megabyte is not one of them.
        char *after = nullptr;
        long want = strtol(lengthText, &after, 10);
        if (want < 0 || want > 1024 * 1024) {
            answer_plain(fd, "413 Payload Too Large", origin);
            goto done;
        }
        // What came in with the headers goes first. Anything past the length is not this
        // request's, and is left behind the way an unread byte would be.
        if (extra > want) extra = (int) want;
        if (extra > 0 && !tls_write_all(tls, request + got + 1, extra)) {
            answer_plain(fd, "502 Bad Gateway", origin);
            goto done;
        }
        want -= extra;
        while (want > 0) {
            char chunk[RELAY_CHUNK];
            long take = want < RELAY_CHUNK ? want : RELAY_CHUNK;
            if (!readable(fd, CLIENT_IDLE_SECONDS)) goto done;
            int n = recv(fd, chunk, (int) take, 0);
            if (n <= 0) goto done;
            if (!tls_write_all(tls, chunk, n)) {
                answer_plain(fd, "502 Bad Gateway", origin);
                goto done;
            }
            want -= n;
        }

        bool more = false;
        {
            char answer[HEADER_MAX];
            int answerExtra = 0;
            int answerLen = tls_read_headers(tls, up, answer, sizeof(answer), &answerExtra, &more);
            if (answerLen <= 0) {
                answer_plain(fd, "502 Bad Gateway", origin);
                goto done;
            }
            int headLen = rewrite_response(answer, request, sizeof(request), origin);
            if (!headLen) {
                answer_plain(fd, "502 Bad Gateway", origin);
                goto done;
            }
            if (!send_all(fd, request, headLen)) goto done;
            if (answerExtra > 0 && !send_all(fd, answer + answerLen + 1, answerExtra)) goto done;
        }

        // Relayed as it arrives. The back channel is held open and written down later, so
        // reading it to the end first would hand the page a channel that had already closed.
        for (;;) {
            char chunk[RELAY_CHUNK];
            int32_t n = 0;
            // A read that filled the buffer says there is probably more already decrypted, so
            // the wait is skipped rather than asking the socket about data NSSL is holding.
            if (!more && !readable(up, UPSTREAM_IDLE_SECONDS)) break;
            if (NSSLRead(tls, chunk, sizeof(chunk), &n) < 0 || n <= 0) break;
            more = (n == (int32_t) sizeof(chunk));
            if (!send_all(fd, chunk, n)) break;
        }
    }

done:
    if (tls >= 0) {
        std::lock_guard<std::mutex> hold(sNsslLock);
        NSSLDestroyConnection(tls);
    }
    live_close(slot);
}

// pthread rather than std::thread: this build has no exceptions, so a std::thread that cannot
// start calls terminate and takes the app with it, and running out of memory part way through a
// video is exactly when that would happen.
static void *worker(void *arg) {
    serve((int) (intptr_t) arg);
    return nullptr;
}

// Detached once it is made. This newlib's pthread_create takes only the stack from its attributes,
// so a detached state set there is ignored, and a thread nobody joins or detaches never gives its
// stack back.
static bool start_worker(int slot) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, WORKER_STACK);
    pthread_t id;
    int rc = pthread_create(&id, &attr, worker, (void *) (intptr_t) slot);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    pthread_detach(id);
    return true;
}

static void accept_loop() {
    int failures = 0;
    while (sRunning.load()) {
        struct sockaddr_in from{};
        socklen_t len = sizeof(from);
        int listener = sListen.load();
        if (listener < 0) break;
        int fd = accept(listener, (struct sockaddr *) &from, &len);
        if (fd < 0) {
            if (!sRunning.load()) break;
            // An error that does not clear, out of descriptors being the likely one, would
            // otherwise spin this loop on a core for as long as the app runs.
            if (++failures > 64) {
                DEBUG_FUNCTION_LINE("WiiULeanback: the proxy cannot accept, giving up on it");
                break;
            }
            OSSleepTicks(OSMillisecondsToTicks(100));
            continue;
        }
        failures = 0;
        // The stop knocks on the port to wake this, so a connection that arrives after the flag
        // went down is that knock and nothing else.
        if (!sRunning.load()) { close(fd); break; }
        int slot = live_claim(fd);
        if (slot < 0) {
            // Answered rather than dropped. A closed connection reaches the page as a bare
            // network error it can say nothing about, and this is the one path where it would
            // not even carry the headers needed to read a reply.
            answer_busy(fd);
            close(fd);
            continue;
        }
        if (!start_worker(slot)) {
            DEBUG_FUNCTION_LINE("WiiULeanback: the proxy has no thread for a connection");
            trace_line("proxy has no thread for a connection");
            answer_busy(fd);
            live_close(slot);
        }
    }
    sUp = false;
}

static void *accept_thread(void *) {
    accept_loop();
    return nullptr;
}

// Joinable, since the stop waits on it before the listener goes.
static bool start_accepting() {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return false;
    pthread_attr_setstacksize(&attr, ACCEPT_STACK);
    int rc = pthread_create(&sAccept, &attr, accept_thread, nullptr);
    pthread_attr_destroy(&attr);
    return rc == 0;
}

bool proxy_start() {
    if (NSSLInit() < 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: no nssl, the proxy cannot reach youtube");
        return false;
    }
    sTls = NSSLCreateContext(0);
    if (sTls < 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: no tls context, %d", (int) sTls);
        NSSLFinish();
        return false;
    }
    // The root behind Google's chain today, through GlobalSign cross signing GTS Root R1. The
    // other two GlobalSign roots go on as well, since which one is offered is not ours to decide.
    NSSLAddServerPKI(sTls, NSSL_SERVER_CERT_GLOBALSIGN_ROOT_CA);
    NSSLAddServerPKI(sTls, NSSL_SERVER_CERT_GLOBALSIGN_ROOT_CA_R2);
    NSSLAddServerPKI(sTls, NSSL_SERVER_CERT_GLOBALSIGN_ROOT_CA_R3);

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        DEBUG_FUNCTION_LINE("WiiULeanback: the proxy has no socket");
        goto fail;
    }
    {
        int on = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in here{};
        here.sin_family = AF_INET;
        here.sin_port = htons(PROXY_PORT);
        // The page is the only thing that ever asks, and it asks from the console. Listening on
        // every interface would make this a forward proxy for anything else on the network.
        here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, (struct sockaddr *) &here, sizeof(here)) < 0) {
            DEBUG_FUNCTION_LINE("WiiULeanback: the proxy cannot take port %d", PROXY_PORT);
            close(listener);
            goto fail;
        }
    }
    if (listen(listener, MAX_CONNECTIONS) < 0) {
        close(listener);
        goto fail;
    }
    sListen = listener;
    sRunning = true;
    sUp = true;
    if (!start_accepting()) {
        DEBUG_FUNCTION_LINE("WiiULeanback: the proxy has no thread to accept on");
        sRunning = false;
        sUp = false;
        sListen = -1;
        close(listener);
        goto fail;
    }
    sAccepting = true;
    DEBUG_FUNCTION_LINE("WiiULeanback: proxy up on " PROXY_HOST ":%d", PROXY_PORT);
    return true;

fail:
    NSSLDestroyContext(sTls);
    sTls = -1;
    NSSLFinish();
    return false;
}

// Connects to the port and drops it, which is what gets the accept above to return. Closing the
// socket under it is not something this stack promises to wake it from.
static void knock() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_in here{};
    here.sin_family = AF_INET;
    here.sin_port = htons(PROXY_PORT);
    here.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    connect(fd, (struct sockaddr *) &here, sizeof(here));
    close(fd);
}

void proxy_stop() {
    sRunning = false;
    sUp = false;
    if (sAccepting) {
        knock();
        pthread_join(sAccept, nullptr);
        sAccepting = false;
    }
    int listener = sListen.exchange(-1);
    if (listener >= 0) {
        shutdown(listener, SHUT_RDWR);
        close(listener);
    }

    // The workers are detached and most of them are sitting on a read that nothing else would
    // end. Breaking their sockets is what gets them out, and they have to be out before the
    // context they are using goes, because the plugin's code goes with it.
    live_break_all();
    int waited = 0;
    while (live_count() > 0 && waited < DRAIN_SECONDS * 20) {
        OSSleepTicks(OSMillisecondsToTicks(50));
        waited++;
    }
    if (live_count() > 0) {
        // Freeing what they are still inside would be worse than keeping it. The context stays,
        // and so does nssl.
        DEBUG_FUNCTION_LINE("WiiULeanback: %d proxy connections did not end, leaving tls up",
                            live_count());
        return;
    }
    if (sTls >= 0) {
        NSSLDestroyContext(sTls);
        sTls = -1;
    }
    NSSLFinish();
}
