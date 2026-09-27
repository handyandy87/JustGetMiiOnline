// The page needs to know the version, for the 2013 build its arrangement of the screens and the
// suggestion strip, and for both builds the comments, before the app draws. Asking the listener for them that
// early means a synchronous read that holds the launch up. The url the app was started at is the
// one thing the page always has, so they ride on it:
//
//     https://www.youtube.com/tv?lb=dual,suggest,comments
//     https://www.youtube.com/tv?lb=2016,comments
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
#include "patch.h"
#include "settings.h"
#include "trace.h"
#include "utils/logger.h"

// The pointer and the string it points at, as linked.
#define POINTER_LINK 0x103790e0u
#define URL_LINK     0x10000188u

static const char OWN_URL[] = "https://www.youtube.com/tv";

static char sUrl[96];

const char *start_url_point() {
    const char *screens = settings_screens_name();
    if (screens) {
        snprintf(sUrl, sizeof(sUrl), "%s?lb=%s%s%s", OWN_URL, screens,
                 settings_suggestions() ? ",suggest" : "",
                 settings_comments() ? ",comments" : "");
    } else {
        snprintf(sUrl, sizeof(sUrl), "%s?lb=2016%s", OWN_URL,
                 settings_comments() ? ",comments" : "");
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
    DEBUG_FUNCTION_LINE("WiiULeanback: the app starts at %s", sUrl);
    return sUrl;
}
