#pragma once

// The page's Share button. It posts the video it's on to the listener, which checks who's asking
// and hands the video to the plugin's post screen.
//
//     POST /__leanback_share
//     Origin: https://www.youtube.com
//     Content-Type: application/x-www-form-urlencoded
//
//     v=<11 char id>&t=<title>&c=<channel>&k=<token>
//
// The checks run in this order: the method, then Origin, or Referer when there's no Origin, then
// the body's length, then the token, then 6 requests a minute, and only then the fields. The
// token is made at every start and reaches the page on its start url and nowhere else.
//
// While the post screen is up the page asks how it's doing, with the same checks and a cap of
// its own, 90 a minute:
//
//     GET /__leanback_share?k=<token>
//
// and gets idle, opening, open or failed as plain text.
//
// share.cpp is plain C++ that builds on the Mac too. share_token.cpp makes the token on the
// console, and proxy_share and proxy_share_state come from the plugin's own glue.

#include <cstddef>
#include <cstdint>
#include <mutex>

#define SHARE_PATH "/__leanback_share"
// The one page allowed to ask, scheme and host.
#define SHARE_ORIGIN "https://www.youtube.com"
#define SHARE_BODY_MAX 1024
#define SHARE_ID_BYTES 11
#define SHARE_TITLE_MAX 300
#define SHARE_CHANNEL_MAX 100
// What share_token hands out: hex, and never shorter than SHARE_TOKEN_MIN.
#define SHARE_TOKEN_MIN 16
#define SHARE_TOKEN_MAX 32
#define SHARE_PER_MINUTE 6
#define SHARE_STATE_PER_MINUTE 90

// The token for this start, or "" when none could be made, which every request then fails on.
// Nothing may log, trace or keep a copy of it.
const char *share_token();

// Makes a new token. Call from the application start hook, before the listener comes up and
// before the start url is built.
void share_token_new();

// Opens the post screen for a video. Answers the status the page gets: 202 when the screen is
// opening, 409 when it's busy, 503 when there's no screen to open and 400 for input it won't
// take. channel is never null. It runs on a listener worker, one call at a time, so it has to
// answer right away rather than wait on the screen. Each plugin's glue has its own.
extern int proxy_share(const char *video_id, const char *title, const char *channel);

// Where the post screen is: 0 idle, 1 opening while it loads, 2 open while it's up, 3 failed when
// it couldn't open. Answered on a listener worker, so it has to be quick. Each plugin's glue has
// its own.
#define SHARE_STATE_IDLE 0
#define SHARE_STATE_OPENING 1
#define SHARE_STATE_OPEN 2
#define SHARE_STATE_FAILED 3
extern int proxy_share_state(void);

// The requests a minute lets through, up to limit, as times in milliseconds off a clock that
// only goes up.
struct ShareRate {
    explicit ShareRate(int perMinute = SHARE_PER_MINUTE)
        : limit(perMinute < 1 ? 1
                : perMinute > SHARE_STATE_PER_MINUTE ? SHARE_STATE_PER_MINUTE
                                                     : perMinute) {}
    std::mutex lock;
    const int limit;
    uint64_t at[SHARE_STATE_PER_MINUTE] = {};
    int next = 0;
    int used = 0;
};

// What the form came to, cut to size.
struct ShareFields {
    char v[SHARE_ID_BYTES + 1];
    char t[SHARE_TITLE_MAX + 1];
    char c[SHARE_CHANNEL_MAX + 1];
};

// Whether Origin, or Referer when Origin is missing or empty, is exactly https://www.youtube.com,
// with the host in any case. A Referer may go on with a path; an Origin may not. Either may be
// null.
bool share_origin_ok(const char *origin, const char *referer);

// Whether given, length bytes of it, is token, in time that doesn't depend on where they differ.
// An empty or short token matches nothing.
bool share_token_equal(const char *given, size_t length, const char *token);

// Whether bytes is UTF-8 with no control character in it, below 0x20 or 0x7f.
bool share_text_ok(const char *bytes, size_t length);

// How many of bytes to keep for at most max, cut where a character starts. bytes has to be valid
// UTF-8.
size_t share_text_cut(const char *bytes, size_t length, size_t max);

// Takes one request out of the minute at now, or answers false when the limit have gone in the
// last 60,000 ms.
bool share_rate_take(ShareRate *rate, uint64_t nowMs);

// What the request's headers alone decide: the method, Origin or Referer and Content-Length.
// headers is the whole block, request line first, ending in a NUL. Answers 0 with the body's
// length in *length when the body is worth reading, or 405, 403, 413 or 400.
int share_head(const char *headers, size_t *length);

// What the body decides: a malformed form is 400, the token 403, the cap 429, a bad field 400,
// and past those whatever proxy_share answers, one of its four or 503. fields gets what was
// handed to proxy_share, and may be null.
int share_body(ShareRate *rate, const char *body, size_t length, uint64_t nowMs,
               ShareFields *fields);

// The GET: the method, Origin or Referer, the token in the query, the cap, and then
// proxy_share_state. headers is the whole block, request line first. Answers 200 with *text set
// to idle, opening, open or failed, or 405, 403, 400, 429, or 503 for a state it doesn't know. *text is
// "" on anything but 200.
int share_state(ShareRate *rate, const char *headers, uint64_t nowMs, const char **text);

// The word for a state, or null for one that isn't one of the four.
const char *share_state_text(int state);

// The status line for one of the statuses above, "503 Service Unavailable" for any other.
const char *share_status_line(int status);
