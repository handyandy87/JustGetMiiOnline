/*  Copyright 2026 HandyAndy87

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// The YouTube Patcher: my WiiULeanback plugin's patches for the Wii U YouTube app, run from this
// one.
//
// The app stopped starting when Nintendo's service token endpoint began answering "Service has
// expired" for its client id, and youtube.com stopped serving anything it can run. The patcher
// reports that token as issued, answers www.youtube.com with a revival host, puts the host's root
// beside the app's own certificates, and patches the app in memory so the page can reach Google
// for itself. It acts in the YouTube app and nowhere else, and only with YouTube Patcher on.
//
// Everything under youtube/ is WiiULeanback's source, byte for byte. Change it there and copy it
// over, never here alone. This file is the rest of WiiULeanback, rewritten for this plugin: the
// menu and storage behind youtube/settings.h, the certificate, what runs when the app starts and
// ends, the name answer dns.cpp asks for, and the service token fix.

#include "youtube.h"
#include "logger.h"
#include "youtube/image.h"
#include "youtube/newline.h"
#include "youtube/proxy.h"
#include "youtube/revival_ca.h"
#include "youtube/settings.h"
#include "youtube/start.h"
#include "youtube/tls12.h"
#include "youtube/ua.h"
#include "youtube/whitelist.h"

#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemMultipleValues.h>
#include <wups/config_api.h>
#include <wups/storage.h>

#include <content_redirection/redirection.h>
#include <coreinit/title.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <cstring>

// Where the revival host's root goes on the card. The layer's root holds nothing but ssl/certs,
// because a ContentRedirection that won't layer one folder gets the layer over all of
// /vol/content instead, and anything else in here would turn up among the app's own files.
#define CONFIG_DIR "fs:/vol/external01/wiiu/environments/aroma/plugins/config"
#define PLUGIN_DIR CONFIG_DIR "/JustGetMiiOnline"
#define LAYER_ROOT PLUGIN_DIR "/youtube"
#define CERT_DIR LAYER_ROOT "/ssl/certs"
#define CERT_PATH CERT_DIR "/" REVIVAL_CA_FILENAME

// The folder the app sets SSL_CERT_DIR to, which is where it looks up every root.
#define CERT_TARGET "/vol/content/ssl/certs"

namespace {

constexpr const char *KEY_PATCHER = "youtube_patcher";
constexpr const char *KEY_VERSION = "youtube_version";
constexpr const char *KEY_SUGGESTIONS = "youtube_suggestions";
constexpr const char *KEY_COMMENTS = "youtube_comments";

constexpr const char *YOUTUBE_CLIENT_ID = "e921a604fce89365498613fdf001b492";
constexpr uint64_t YOUTUBE_TITLE_ID = 0x0005000010105700llu;      // the YouTube app
constexpr uint64_t YOUTUBE_BETA_TITLE_ID = 0x000500001014CE00llu; // the YouTube Beta app

// The name the app asks for, and where it should end up. An address rather than a name, because
// resolving one here would mean asking the resolver dns.cpp stands in front of.
constexpr const char *REVIVAL_HOSTNAME = "www.youtube.com";
constexpr const char *REVIVAL_ADDRESS = "198.23.134.113";

bool patcher = true;
Version version = VERSION_2013_STANDARD;
bool suggestions = true;
bool comments = true;

// In the order the menu lists them, which isn't the order of their values. Same names and order
// as WiiULeanback's menu.
constexpr std::array<ConfigItemMultipleValuesPair, 5> VERSION_VALUES = {{
    {static_cast<uint32_t>(VERSION_CURRENT), "Current (Official)"},
    {static_cast<uint32_t>(VERSION_2012_LAUNCH), "2012 (Launch)"},
    {static_cast<uint32_t>(VERSION_2013_STANDARD), "2013 (Standard)"},
    {static_cast<uint32_t>(VERSION_2013_DUAL), "2013 (Mirrored Screen)"},
    {static_cast<uint32_t>(VERSION_2016), "2016 (Standard)"},
}};

// The menu takes a position in the list above and hands back the value.
int version_position(Version wanted) {
    for (size_t i = 0; i < VERSION_VALUES.size(); ++i) {
        if (VERSION_VALUES[i].value == static_cast<uint32_t>(wanted)) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// The 2013 build's arrangements, by the names the page knows them by.
const char *screens_of(Version wanted) {
    switch (wanted) {
        case VERSION_2012_LAUNCH: return "hybrid";
        case VERSION_2013_STANDARD: return "steel";
        case VERSION_2013_DUAL: return "dual";
        default: return nullptr;
    }
}

void log_storage(WUPSStorageError err, const char *what) {
    if (err != WUPS_STORAGE_ERROR_SUCCESS) {
        LOG("storage %s: %s", what, WUPSStorageAPI_GetStatusStr(err));
    }
}

// Numbers, like every other setting in this plugin's storage. A missing value is stored as the
// default, and an unreadable one falls back to it.
uint32_t load(const char *key, uint32_t fallback) {
    uint32_t stored = fallback;
    const WUPSStorageError err = WUPSStorageAPI_GetU32(nullptr, key, &stored);
    if (err == WUPS_STORAGE_ERROR_NOT_FOUND) {
        log_storage(WUPSStorageAPI_StoreU32(nullptr, key, fallback), key);
        return fallback;
    }
    if (err != WUPS_STORAGE_ERROR_SUCCESS) {
        log_storage(err, key);
        return fallback;
    }
    return stored;
}

void store(const char *key, uint32_t value) {
    log_storage(WUPSStorageAPI_StoreU32(nullptr, key, value), key);
}

void patcher_changed(ConfigItemBoolean *, bool value) {
    patcher = value;
    store(KEY_PATCHER, value ? 1u : 0u);
}

void version_changed(ConfigItemMultipleValues *, uint32_t value) {
    // The item only offers the five. The check is here because the number came from outside
    // this file.
    if (value > static_cast<uint32_t>(VERSION_CURRENT)) {
        LOG("ignoring an unrecognized YouTube version: %u", static_cast<unsigned>(value));
        return;
    }
    version = static_cast<Version>(value);
    store(KEY_VERSION, value);
}

void suggestions_changed(ConfigItemBoolean *, bool value) {
    suggestions = value;
    store(KEY_SUGGESTIONS, value ? 1u : 0u);
}

void comments_changed(ConfigItemBoolean *, bool value) {
    comments = value;
    store(KEY_COMMENTS, value ? 1u : 0u);
}

bool is_youtube_title() {
    const uint64_t id = OSGetTitleID();
    return id == YOUTUBE_TITLE_ID || id == YOUTUBE_BETA_TITLE_ID;
}

// Writes the root to the card unless it's already there whole, and says whether it's in place.
// Only ever from YouTube's own start. logger.h has what the card cost this plugin at the starts
// that come during boot.
bool deploy_certificate() {
    struct stat st{};
    if (stat(CERT_PATH, &st) == 0 && st.st_size == static_cast<off_t>(sizeof(REVIVAL_CA_PEM) - 1)) {
        return true;
    }
    mkdir(PLUGIN_DIR, 0777);
    mkdir(LAYER_ROOT, 0777);
    mkdir(LAYER_ROOT "/ssl", 0777);
    mkdir(CERT_DIR, 0777);
    FILE *file = std::fopen(CERT_PATH, "wb");
    if (!file) {
        LOG("YouTube: could not write %s", CERT_PATH);
        return false;
    }
    const size_t want = sizeof(REVIVAL_CA_PEM) - 1;
    const bool written = std::fwrite(REVIVAL_CA_PEM, 1, want, file) == want;
    std::fclose(file);
    return written;
}

// Whether this run is one the patcher started, and whether it's Current (Official), which nothing
// is redirected for. Both are set at the start and read by everything after it, so the name
// answer and the end follow what the start set up whatever the menu says by then.
bool active = false;
bool hosted = false;

bool redirection = false;
CRLayerHandle cert_layer = 0;

// What the last start did, for the Debug page. The run log is WiiULeanback's alone, so this is
// the one place a result can be read here.
struct Report {
    bool ran;
    Version version;
    const char *image;
    bool tls;
    bool newline_tried;
    bool newline;
    bool signin;
    bool agent;
    bool url;
    bool proxy;
    const char *certificate;
};
Report report = {};

} // namespace

void settings_load() {
    patcher = load(KEY_PATCHER, 1) != 0;

    const uint32_t stored = load(KEY_VERSION, static_cast<uint32_t>(VERSION_2013_STANDARD));
    if (stored > static_cast<uint32_t>(VERSION_CURRENT)) {
        LOG("unrecognized stored YouTube version %u, using 2013 (Standard)",
            static_cast<unsigned>(stored));
        version = VERSION_2013_STANDARD;
    } else {
        version = static_cast<Version>(stored);
    }

    suggestions = load(KEY_SUGGESTIONS, 1) != 0;
    comments = load(KEY_COMMENTS, 1) != 0;
}

// None of these relaunch the console. They're read when YouTube starts, so a change takes effect
// the next time it does.
bool settings_menu(WUPSConfigCategoryHandle root) {
    if (WUPSConfigItemBoolean_AddToCategoryEx(root, KEY_PATCHER, "YouTube Patcher", true, patcher,
                                              &patcher_changed, "Enabled",
                                              "Disabled") != WUPSCONFIG_API_RESULT_SUCCESS) {
        return false;
    }
    if (WUPSConfigItemMultipleValues_AddToCategory(
            root, KEY_VERSION, "App Version Experience", version_position(VERSION_2013_STANDARD),
            version_position(version),
            const_cast<ConfigItemMultipleValuesPair *>(VERSION_VALUES.data()),
            VERSION_VALUES.size(), &version_changed) != WUPSCONFIG_API_RESULT_SUCCESS) {
        return false;
    }
    if (WUPSConfigItemBoolean_AddToCategoryEx(root, KEY_SUGGESTIONS, "Search Suggestions", true,
                                              suggestions, &suggestions_changed, "Enabled",
                                              "Disabled") != WUPSCONFIG_API_RESULT_SUCCESS) {
        return false;
    }
    if (WUPSConfigItemBoolean_AddToCategoryEx(root, KEY_COMMENTS,
                                              "Use GamePad for Comments/Video Info", true,
                                              comments, &comments_changed, "Enabled",
                                              "Disabled") != WUPSCONFIG_API_RESULT_SUCCESS) {
        return false;
    }
    return true;
}

Version settings_version() {
    return version;
}

// The run log stays WiiULeanback's, so nothing under youtube/ ever writes one from here.
bool settings_trace() {
    return false;
}

bool settings_suggestions() {
    return suggestions && screens_of(version);
}

bool settings_comments() {
    return comments && (screens_of(version) || version == VERSION_2016);
}

const char *settings_screens_name() {
    return screens_of(version);
}

void youtube_start() {
    if (!is_youtube_title()) {
        return;
    }
    // A start with the patcher off clears the readout too, so the Debug page never describes a
    // run from earlier in the boot as the last one.
    active = patcher;
    report = {};
    if (!active) {
        return;
    }
    report.ran = true;
    report.version = version;

    // Every patch below works from where the loader put the rpx, which is not where it was linked.
    image_locate();
    report.image = image_how();

    // youtube.com serves YouTube's own app at the app's own start url, and the patcher keeps out of
    // it but for the User-Agent, which youtube.com answers 404 without. The token fix still
    // answers, since the app stops at "Service has expired" before it asks youtube.com for
    // anything.
    hosted = version == VERSION_CURRENT;
    if (hosted) {
        report.agent = ua_unblock();
        return;
    }

    report.tls = tls12_enable();
    // Only the comments' typing tells Return from OK, so nothing else sees the change.
    if (settings_comments()) {
        report.newline_tried = true;
        report.newline = newline_apart();
    }
    report.signin = whitelist_allow_accounts();
    report.agent = ua_unblock();
    report.url = start_url_point() != nullptr;
    report.proxy = proxy_start();

    if (!deploy_certificate()) {
        report.certificate = "not on the card";
        return;
    }
    if (ContentRedirection_InitLibrary() != CONTENT_REDIRECTION_RESULT_SUCCESS) {
        report.certificate = "without ContentRedirection";
        return;
    }
    redirection = true;

    // MERGE rather than REPLACE, so the app's own 68 roots still load and only this one is added.
    report.certificate = "over ssl/certs";
    ContentRedirectionStatus layer = ContentRedirection_AddFSLayerEx(
        &cert_layer, "JustGetMiiOnline certificate", CERT_TARGET, CERT_DIR,
        FS_LAYER_TYPE_EX_MERGE_DIRECTORY);
    if (layer != CONTENT_REDIRECTION_RESULT_SUCCESS) {
        report.certificate = "over /vol/content";
        layer = ContentRedirection_AddFSLayer(&cert_layer, "JustGetMiiOnline certificate",
                                              LAYER_ROOT, FS_LAYER_TYPE_CONTENT_MERGE);
    }
    if (layer != CONTENT_REDIRECTION_RESULT_SUCCESS) {
        LOG("YouTube: could not add the certificate layer, %d", static_cast<int>(layer));
        report.certificate = "refused by ContentRedirection";
    }
}

void youtube_end() {
    if (!active) {
        return;
    }
    active = false;
    if (!hosted) {
        proxy_stop();
    }
    if (cert_layer) {
        ContentRedirection_RemoveFSLayer(cert_layer);
        cert_layer = 0;
    }
    // The module is shared and counts its users, so one of these without a matching init pulls it
    // out from under whatever else on the console is holding it.
    if (redirection) {
        ContentRedirection_DeInitLibrary();
        redirection = false;
    }
}

const char *youtube_answer(const char *node) {
    if (!active || !node || !is_youtube_title()) {
        return node;
    }
    if (!hosted && std::strcmp(node, REVIVAL_HOSTNAME) == 0) {
        return REVIVAL_ADDRESS;
    }
    // The proxy's own name, only while it's listening. When it isn't, the name doesn't resolve
    // and the page falls back on whoever served it.
    if (std::strcmp(node, PROXY_HOST) == 0 && proxy_running()) {
        return "127.0.0.1";
    }
    return node;
}

// Three lines at most: the version and how the app was found in memory, which patches took, and
// the proxy and certificate. A patch that didn't take reads "no" in front of its name. Current
// (Official) only gets the User-Agent, so it only gets the first two.
bool youtube_report_line(size_t index, char *out, size_t size) {
    if (!report.ran) {
        if (index != 0 || !patcher) {
            return false;
        }
        std::snprintf(out, size, "YouTube: not run with the patcher yet");
        return true;
    }

    const auto took = [](bool done) { return done ? "" : "no "; };
    switch (index) {
        case 0:
            std::snprintf(out, size, "YouTube: %s, located by %s",
                          VERSION_VALUES[version_position(report.version)].valueName,
                          report.image);
            return true;
        case 1:
            if (report.version == VERSION_CURRENT) {
                std::snprintf(out, size, "YouTube patches: %sagent", took(report.agent));
                return true;
            }
            std::snprintf(out, size, "YouTube patches: %stls, %sagent, %ssign-in, %surl%s%s",
                          took(report.tls), took(report.agent), took(report.signin),
                          took(report.url), report.newline_tried ? ", " : "",
                          report.newline_tried ? (report.newline ? "newline" : "no newline")
                                               : "");
            return true;
        case 2:
            if (report.version == VERSION_CURRENT) {
                return false;
            }
            std::snprintf(out, size, "YouTube proxy %s, certificate %s",
                          report.proxy ? "listening" : "down",
                          report.certificate ? report.certificate : "not tried");
            return true;
    }
    return false;
}

// Nintendo answers "Service has expired" for YouTube's client id and the app gives up on it,
// whichever YouTube it was about to start. The token itself is never used, so reporting success
// is enough. GiveMiiYouTube's fix, MIT.
DECL_FUNCTION(int, aist_youtube, uint8_t *token, const char *client_id) {
    if (patcher && client_id && std::strcmp(client_id, YOUTUBE_CLIENT_ID) == 0) {
        return 0;
    }
    return real_aist_youtube(token, client_id);
}

// The two argument call the YouTube app makes, not the five argument one token.cpp answers
// Miiverse on.
WUPS_MUST_REPLACE_FOR_PROCESS(aist_youtube, WUPS_LOADER_LIBRARY_NN_ACT,
                              AcquireIndependentServiceToken__Q2_2nn3actFPcPCc,
                              WUPS_FP_TARGET_PROCESS_GAME_AND_MENU);
