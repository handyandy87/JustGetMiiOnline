// The page needs to know the version, for the 2013 build its arrangement of the screens and the
// suggestion strip, and for both builds the comments, before the app draws. Asking the listener for them that
// early means a synchronous read that holds the launch up. The url the app was started at is the
// one thing the page always has, so they ride on it:
//
//     https://www.youtube.com/tv?lb=dual,suggest,comments
//     https://www.youtube.com/tv?lb=2016,comments
//
// A run started from a Miiverse post that names a video gets the watch route after the query,
// which both apps open at load:
//
//     https://www.youtube.com/tv?lb=2016,comments#/watch?v=<id>
//
// With Share on, the list gets share and the query gets the listener's token for this start,
// which the page sends back with every share:
//
//     https://www.youtube.com/tv?lb=dual,comments,share&shtok=<token>
//
// The token is a secret, and the copy for the logs doesn't have it.
//
// The host serves the 2016 app for a url naming 2016 and the 2013 one otherwise. Neither app reads
// a query parameter it doesn't know, and each app's own client reads this one. The 2016 app is
// told no arrangement, since the one it draws is its own.
//
// The shell reads its start url through a pointer that is the first word of .data, and the
// string it points at sits in a slot with room for nothing longer. So rather than rewrite the
// string, the pointer is pointed at a url of the plugin's own, which stays put for as long as the
// plugin is loaded.

#include "start.h"

#include <cstdio>
#include <cstring>
#include <whb/log.h>

#include "image.h"
#include "jump.h"
#include "patch.h"
#include "settings.h"
#include "share.h"
#include "trace.h"
#include "utils/logger.h"

// The pointer and the string it points at, as linked.
#define POINTER_LINK 0x103790e0u
#define URL_LINK     0x10000188u

static const char OWN_URL[] = "https://www.youtube.com/tv";

static const char WATCH[] = "#/watch?v=";
static const char TOKEN[] = "&shtok=";

// The longest url: the host, the longest version name, suggest, comments, share, the longest
// token and the watch route.
static char sUrl[sizeof(OWN_URL) + sizeof("?lb=hybrid,suggest,comments,share") + sizeof(TOKEN) +
                 SHARE_TOKEN_MAX + sizeof(WATCH) + JUMP_ID_BYTES];
static char sShown[sizeof(sUrl)];

// Builds the url into sUrl and a copy for the logs into sShown, or leaves both empty when it
// doesn't fit.
static void buildUrl() {
    const char *screens = settings_screens_name();
    const char *suggest = screens && settings_suggestions() ? ",suggest" : "";
    const char *comments = settings_comments() ? ",comments" : "";
    // A token shorter than share_token promises is one that couldn't be made, and the button
    // stays off.
    const char *token = share_token();
    size_t tokenLength = strnlen(token, SHARE_TOKEN_MAX + 1);
    bool hasShare = settings_share() && settings_version() != VERSION_CURRENT &&
                    tokenLength >= SHARE_TOKEN_MIN && tokenLength <= SHARE_TOKEN_MAX;
    const char *share = hasShare ? ",share" : "";
    const char *tokenName = hasShare ? TOKEN : "";
    const char *video = jump_video_id();
    const char *watch = video ? WATCH : "";
    if (!video) {
        video = "";
    }
    int n = snprintf(sUrl, sizeof(sUrl), "%s?lb=%s%s%s%s%s%s%s%s", OWN_URL,
                     screens ? screens : "2016", suggest, comments, share, tokenName,
                     hasShare ? token : "", watch, video);
    sShown[0] = 0;
    if (n <= 0 || (size_t) n >= sizeof(sUrl)) {
        sUrl[0] = 0;
        return;
    }
    snprintf(sShown, sizeof(sShown), "%s?lb=%s%s%s%s%s%s%s%s", OWN_URL,
             screens ? screens : "2016", suggest, comments, share, tokenName,
             hasShare ? "..." : "", watch, video);
}

const char *start_url_point() {
    buildUrl();
    if (!sUrl[0]) {
        trace_line("start url did not fit, left as the app's own");
        return nullptr;
    }

    uint32_t pointer = image_data(POINTER_LINK);
    uint32_t mine = (uint32_t) sUrl;
    uint32_t now = 0;
    if (!patch_read(pointer, &now, sizeof(now))) {
        trace_line("start url pointer 0x%08x has no page behind it", pointer);
        return nullptr;
    }
    if (now == mine) {
        return sUrl;
    }
    // Only a pointer that still points at the app's own url is taken over, so a layout that is
    // not the one this was read from is left alone rather than written over.
    char was[sizeof(OWN_URL)];
    if (now != image_data(URL_LINK) || !patch_read(now, was, sizeof(was)) ||
        memcmp(was, OWN_URL, sizeof(OWN_URL)) != 0) {
        trace_line("start url pointer 0x%08x holds 0x%08x, not the app's own url", pointer, now);
        return nullptr;
    }
    if (!patch_write(pointer, &mine, sizeof(mine))) {
        trace_line("start url pointer 0x%08x found, the write did not take", pointer);
        return nullptr;
    }
    DEBUG_FUNCTION_LINE("WiiULeanback: the app starts at %s", sShown);
    return sUrl;
}
