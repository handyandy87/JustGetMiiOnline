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

// Share to Miiverse, the plugin's half. The page's button asks the YouTube Patcher's listener,
// the listener calls olv_share_start, and this opens the console's own post screen with the
// video's title and link in it.
//
// Everything about the library is taken from the Wii U browser engine I build in
// wiiu3dshomebrew, which posts through the same calls: the export names, the sizes, the order
// of the setters and the answers they give. What's different here is the app. YouTube doesn't
// import nn_olv, isn't a title Miiverse set up for, and runs this plugin's code on a listener
// thread with a 64 KB stack, so the library is loaded here, set up here once, and only ever
// called from a thread of this file's own with the stack the engine gives it.
//
// It never calls the account or token functions. The library asks for its own token inside the
// process, through the same call token.cpp's game-process hook answers for the selected
// Miiverse, and the post screen it switches to is the Miiverse applet, which olv.cpp and
// token.cpp already cover.
//
// ACPAssignTitlePatch is left out on purpose. The engine needs it to post under a title id of
// its own choosing; YouTube is a real title with its own meta.xml and access key, which is what
// the library reads anyway. The Debug page says it was skipped, so a refusal that turns out to
// need it reads as that.

#include "olv_share.h"
#include "config.h"
#include "logger.h"
#include "olv.h"
#include "youtube/share.h"

#include <coreinit/dynload.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <nn/acp/title.h>
#include <nn/act.h>
#include <nsysnet/nssl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <malloc.h>

namespace {

// The community a share posts to on each Miiverse. Protaverse's and Roseverse's are YouTube's
// own: Protaverse's Debug line read 26 back after a share, and eight posts from Roseverse's
// YouTube community page each reported 8. Juxt has no YouTube community, so 800 is its generic
// Homebrew one.
struct Community {
    Miiverse miiverse;
    uint32_t id;
};
constexpr Community COMMUNITIES[] = {
    {Miiverse::Protaverse, 26},
    {Miiverse::Juxt, 800},
    {Miiverse::Roseverse, 8},
};

// What the library answers for success, the one a revival's own build answers with, and the
// one Initialize answers when the process already has it set up.
constexpr uint32_t OLV_OK = 0;
constexpr uint32_t OLV_ROSE_OK = 0x01100080u;
constexpr uint32_t OLV_ALREADY = 0xC1106700u;

// What GetResultByPostApp answers, besides 0 for a post.
constexpr uint32_t OLV_CANCEL_OR_FAILURE = 0xA1109680u;
constexpr uint32_t OLV_NOTHING_BACK = 0xE1106380u;

// Initialize's work buffer and the upload's, one size for both. The library keeps the first
// until Finalize, which never comes, so it's taken once per process and never freed. The
// upload's is only read during UploadPostDataByPostApp, which packs everything it needs into
// the switch's arguments, so it goes as soon as that returns: the engine frees it there too,
// and YouTube's heap can't spare a second megabyte for nothing.
constexpr uint32_t WORK_BYTES = 0x100000;
// The upload parameter's constructor writes about 5.6 KB.
constexpr uint32_t PARAM_BYTES = 0x2000;

// The post screen and the library want more stack than the listener's worker has.
constexpr uint32_t STACK_BYTES = 256 * 1024;
constexpr int32_t THREAD_PRIORITY = 16;

// Between the end of a share that reached the post screen and the next one, the engine's gap.
constexpr uint64_t GAP_MS = 60000;

// The service's limit on a post's text, in UTF-16 units, and the parameter's own maximum
// before it's raised. SetBodyText refuses a longer text rather than cutting it, so the maximum
// goes in first.
constexpr uint32_t TEXT_MAX = 200;
constexpr uint32_t TEXT_DEFAULT_MAX = 100;

constexpr size_t VIDEO_ID_LENGTH = 11;
constexpr const char *LINK_PREFIX = "https://youtu.be/";

// The community probe's buffers, the engine's sizes.
constexpr uint32_t PROBE_PARAM_BYTES = 0x1000;
constexpr uint32_t PROBE_TOPIC_BYTES = 0x1000;
constexpr uint32_t PROBE_POST_BYTES = 0x10000;

// What nn::olv::Initialize takes: 0x40 bytes, the work buffer at 0x08 and its size at 0x0C,
// everything else zero.
struct InitParam {
    uint32_t flags;
    uint32_t report_types;
    void *work;
    uint32_t work_size;
    void *sys_args;
    uint32_t sys_args_size;
    uint8_t pad[0x28];
};
static_assert(sizeof(InitParam) == 0x40);

// A code the share never got as far as asking for.
constexpr uint32_t NOT_RUN = 0xFFFFFFFFu;

enum class State : uint32_t { Idle, Running, Launched, Done };

// The library's calls, looked up by name. Null where it isn't exported.
struct Olv {
    int32_t (*initialize)(const InitParam *);
    void (*construct)(void *);
    uint32_t (*set_work)(void *, uint8_t *, uint32_t);
    uint32_t (*set_community)(void *, uint32_t);
    uint32_t (*set_body_max)(void *, uint32_t);
    uint32_t (*set_body)(void *, const uint16_t *);
    uint32_t (*set_app_data)(void *, const uint8_t *, uint32_t);
    uint32_t (*set_url)(void *, const char *);
    int32_t (*upload)(const void *);
    uint32_t (*get_result)();
    void (*probe_param)(void *);
    uint32_t (*probe_max)(void *, uint32_t);
    void (*probe_topic)(void *);
    void (*probe_post)(void *);
    uint32_t (*probe_download)(void *, void *, uint32_t *, uint32_t, const void *);
    uint32_t (*probe_community)(const void *);
};

// What the last share did, for the Debug page. Written by the share thread and the foreground
// hook, read by the menu, and only ever plain numbers and pointers to literals.
struct Report {
    bool ran;
    char video[VIDEO_ID_LENGTH + 1];
    uint32_t units;
    uint32_t community;
    const char *outcome;
    const char *why;
    uint32_t acquire;
    char hosts[64];
    uint32_t meta;
    uint32_t key;
    uint32_t initialize;
    uint32_t initialize_ms;
    bool initialized_before;
    uint32_t probe;
    uint32_t probe_id;
    uint32_t work;
    uint32_t set_community;
    uint32_t body_max;
    uint32_t body;
    uint32_t app_data;
    uint32_t url;
    uint32_t upload;
    uint32_t result;
};
Report report = {};

// Per process, all of it, and cleared by olv_share_reset. Plugin globals outlive the process, but
// the heap these blocks came from and the library they point into don't.
std::atomic<State> state{State::Idle};
uint64_t finished_ms = 0;
bool finished_once = false;
bool unusable = false;
OSDynLoad_Module library = nullptr;

// The library loaded at application start, on the app's own thread, rather than from the share
// thread: the first console test had the share thread's acquire fail. Kept apart from the
// report, which each share clears, and printed raw, since a failure can read 0xFFFFFFFF, the
// same as NOT_RUN.
bool preload_tried = false;
uint32_t preload_code = 0;
bool acquire_tried = false;
bool resolved = false;
Olv olv = {};
bool services = false;
bool initialized = false;
bool probed = false;
uint32_t probe_code = NOT_RUN;
uint32_t probe_id = NOT_RUN;
uint8_t *init_work = nullptr;
uint8_t *stack = nullptr;
bool thread_made = false;

// The OSThread itself lives in the plugin, so it outlives a process, and thread_made says
// whether what's in it belongs to this one.
alignas(16) OSThread thread;

// The share being worked on, built by olv_share_start before the thread starts so the thread
// never reads the caller's strings.
uint16_t body[TEXT_MAX + 1];
uint32_t body_units = 0;
char video[VIDEO_ID_LENGTH + 1];

uint64_t now_ms() {
    return OSTicksToMilliseconds(OSGetSystemTime());
}

uint32_t community_of(Miiverse miiverse) {
    for (const Community &community : COMMUNITIES) {
        if (community.miiverse == miiverse) {
            return community.id;
        }
    }
    return 0;
}

bool video_id_ok(const char *id) {
    if (id == nullptr || std::strlen(id) != VIDEO_ID_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < VIDEO_ID_LENGTH; ++i) {
        const char c = id[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// One code point of UTF-8, advancing past it. False on anything malformed: a stray
// continuation byte, a sequence cut short, an overlong form, a surrogate, or a value past
// U+10FFFF. A control byte is refused here too, since a title never carries one and the post
// screen has no use for it.
bool next_code_point(const unsigned char *text, size_t &at, uint32_t &point) {
    const unsigned char lead = text[at];
    size_t extra = 0;
    uint32_t minimum = 0;
    if (lead < 0x80) {
        point = lead;
    } else if ((lead & 0xE0) == 0xC0) {
        point = lead & 0x1F;
        extra = 1;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        point = lead & 0x0F;
        extra = 2;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        point = lead & 0x07;
        extra = 3;
        minimum = 0x10000;
    } else {
        return false;
    }
    for (size_t i = 1; i <= extra; ++i) {
        const unsigned char next = text[at + i];
        if ((next & 0xC0) != 0x80) {
            return false;
        }
        point = (point << 6) | (next & 0x3F);
    }
    if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
        return false;
    }
    if (point < 0x20 || point == 0x7F) {
        return false;
    }
    at += extra + 1;
    return true;
}

// The longest channel name kept whole, in UTF-16 units. A longer one is cut to this with an
// ellipsis, so a long channel can't push the title out of the post.
constexpr size_t CHANNEL_MAX = 60;
constexpr const char *UPLOADED_BY = "Uploaded by ";
constexpr uint16_t ELLIPSIS = 0x2026;

// Decodes all of text, so a bad byte past any cut still makes it a bad field, and keeps as many
// whole code points as fit in cap units. Sets cut when something didn't fit.
bool decode(const char *text, uint16_t *out, size_t cap, size_t &units, bool &cut) {
    const auto *bytes = reinterpret_cast<const unsigned char *>(text);
    units = 0;
    cut = false;
    for (size_t at = 0; bytes[at] != '\0';) {
        uint32_t point = 0;
        if (!next_code_point(bytes, at, point)) {
            return false;
        }
        const size_t needs = point >= 0x10000 ? 2 : 1;
        if (cut || units + needs > cap) {
            cut = true;
            continue;
        }
        if (needs == 2) {
            point -= 0x10000;
            out[units++] = static_cast<uint16_t>(0xD800 + (point >> 10));
            out[units++] = static_cast<uint16_t>(0xDC00 + (point & 0x3FF));
        } else {
            out[units++] = static_cast<uint16_t>(point);
        }
    }
    return true;
}

// Keeps the first cap units of a decoded field and ends it with an ellipsis when it was longer,
// stepping back off a high surrogate so a pair is never split.
size_t trim(uint16_t *field, size_t units, size_t cap) {
    if (units <= cap || cap == 0) {
        return units <= cap ? units : 0;
    }
    size_t keep = cap - 1;
    if (keep > 0 && field[keep - 1] >= 0xD800 && field[keep - 1] <= 0xDBFF) {
        --keep;
    }
    field[keep++] = ELLIPSIS;
    return keep;
}

void append(size_t &used, const uint16_t *units, size_t count) {
    std::memcpy(body + used, units, count * sizeof(uint16_t));
    used += count;
}

void append(size_t &used, const char *ascii) {
    for (const char *c = ascii; *c != '\0'; ++c) {
        body[used++] = static_cast<uint16_t>(*c);
    }
}

// The post's text, one line each: the title, "Uploaded by" and the channel, and the link, at
// most TEXT_MAX units. The link is always whole, the channel is whole up to CHANNEL_MAX, and the
// title takes what's left and is the first thing cut. An empty title or channel drops its line.
bool build_body(const char *title, const char *channel, const char *id) {
    static uint16_t title_units[TEXT_MAX + 1];
    static uint16_t channel_units[TEXT_MAX + 1];
    size_t title_length = 0;
    size_t channel_length = 0;
    bool cut = false;
    if (!decode(title, title_units, TEXT_MAX + 1, title_length, cut) ||
        !decode(channel, channel_units, TEXT_MAX + 1, channel_length, cut)) {
        return false;
    }

    const size_t link_units = std::strlen(LINK_PREFIX) + VIDEO_ID_LENGTH;
    size_t room = TEXT_MAX - link_units;
    if (channel_length > 0) {
        channel_length = trim(channel_units, channel_length, CHANNEL_MAX);
        room -= std::strlen(UPLOADED_BY) + channel_length + 1;
    }
    if (title_length > 0) {
        title_length = trim(title_units, title_length, room - 1);
    }

    size_t used = 0;
    if (title_length > 0) {
        append(used, title_units, title_length);
        body[used++] = '\n';
    }
    if (channel_length > 0) {
        append(used, UPLOADED_BY);
        append(used, channel_units, channel_length);
        body[used++] = '\n';
    }
    append(used, LINK_PREFIX);
    append(used, id);
    body[used] = 0;
    body_units = static_cast<uint32_t>(used);
    return true;
}

// When a share last failed short of the post screen, so the page can say so for a while after:
// it may have been suspended, or still be polling, when it happened. 0 for none.
std::atomic<uint32_t> failed_ms{0};
constexpr uint32_t FAILED_HOLD_MS = 10000;

void finish(const char *outcome, const char *why = nullptr) {
    report.outcome = outcome;
    report.why = why;
    failed_ms.store(static_cast<uint32_t>(now_ms()) | 1u);
    state.store(State::Done);
}

// Ends a share that got as far as the post screen, which is what starts the gap. Whatever
// Miiverse's screen made of it, including a cancel, is the user's own doing and no failure.
void finish_after_screen(const char *outcome) {
    finished_ms = now_ms();
    finished_once = true;
    finish(outcome);
    failed_ms.store(0);
}

template <typename Function>
bool find(const char *name, Function &out) {
    void *address = nullptr;
    OSDynLoad_FindExport(library, OS_DYNLOAD_EXPORT_FUNC, name, &address);
    out = reinterpret_cast<Function>(address);
    return out != nullptr;
}

// The required calls, then the rest. Answers the first required name that's missing, or null.
const char *resolve() {
    if (!find("Initialize__Q2_2nn3olvFPCQ3_2nn3olv15InitializeParam", olv.initialize)) {
        return "Initialize";
    }
    if (!find("__ct__Q3_2nn3olv28UploadPostDataByPostAppParamFv", olv.construct)) {
        return "the upload parameter";
    }
    if (!find("SetWork__Q3_2nn3olv28UploadPostDataByPostAppParamFPUcUi", olv.set_work)) {
        return "SetWork";
    }
    if (!find("SetCommunityId__Q3_2nn3olv19UploadPostDataParamFUi", olv.set_community)) {
        return "SetCommunityId";
    }
    if (!find("SetBodyText__Q3_2nn3olv15UploadParamBaseFPCw", olv.set_body)) {
        return "SetBodyText";
    }
    if (!find("UploadPostDataByPostApp__Q2_2nn3olvFPCQ3_2nn3olv28UploadPostDataByPostAppParam",
              olv.upload)) {
        return "UploadPostDataByPostApp";
    }
    // Without the maximum a text under the parameter's own still goes; without the app data
    // the post still goes with nothing for a later jump to read; without the result the post
    // still goes and nothing confirms it. Each reads "not run" on the Debug page.
    find("SetBodyTextMaxLength__Q3_2nn3olv28UploadPostDataByPostAppParamFUi", olv.set_body_max);
    find("SetAppData__Q3_2nn3olv15UploadParamBaseFPCUcUi", olv.set_app_data);
    // The video's address as the post's own link, which is what makes a post a video post: the
    // thumbnail post that launched YouTube carried the video's id where a jump puts the post's,
    // and the Miiverse pages read it from this. Two spellings, the way the engine looks for it.
    if (!find("SetExternalUrl__Q3_2nn3olv15UploadParamBaseFPCc", olv.set_url)) {
        find("SetExternalUrl__Q3_2nn3olv19UploadPostDataParamFPCc", olv.set_url);
    }
    find("GetResultByPostApp__Q2_2nn3olvFv", olv.get_result);
    find("__ct__Q3_2nn3olv25DownloadPostDataListParamFv", olv.probe_param);
    find("SetPostDataMaxNum__Q3_2nn3olv25DownloadPostDataListParamFUi", olv.probe_max);
    find("__ct__Q3_2nn3olv19DownloadedTopicDataFv", olv.probe_topic);
    find("__ct__Q3_2nn3olv18DownloadedPostDataFv", olv.probe_post);
    find("DownloadPostDataList__Q2_2nn3olvFPQ3_2nn3olv19DownloadedTopicDataPQ3_2nn3olv18"
         "DownloadedPostDataPUiUiPCQ3_2nn3olv25DownloadPostDataListParam",
         olv.probe_download);
    find("GetCommunityId__Q3_2nn3olv19DownloadedTopicDataCFv", olv.probe_community);
    return nullptr;
}

// The account, SSL and title services the library leans on, once per process and never
// finalized. YouTube runs the first two itself, and each is counted, so one more user costs
// nothing. The title service is reached by name, so this plugin doesn't import nn_acp into
// every title for the sake of one.
bool start_services() {
    if (services) {
        return true;
    }
    OSDynLoad_Module acp = nullptr;
    if (OSDynLoad_Acquire("nn_acp.rpl", &acp) != OS_DYNLOAD_OK) {
        return false;
    }
    ACPResult (*acp_initialize)() = nullptr;
    OSDynLoad_FindExport(acp, OS_DYNLOAD_EXPORT_FUNC, "ACPInitialize",
                         reinterpret_cast<void **>(&acp_initialize));
    if (acp_initialize == nullptr) {
        return false;
    }
    acp_initialize();
    nn::act::Initialize();
    NSSLInit();
    services = true;
    return true;
}

// The access key in YouTube's own meta.xml, which is what the library posts under. Zero means
// the title has no Miiverse identity and nothing it posts would be accepted.
bool read_key() {
    OSDynLoad_Module acp = nullptr;
    ACPResult (*get_meta)(ACPTitleId, ACPMetaXml *) = nullptr;
    if (OSDynLoad_Acquire("nn_acp.rpl", &acp) == OS_DYNLOAD_OK) {
        OSDynLoad_FindExport(acp, OS_DYNLOAD_EXPORT_FUNC, "ACPGetTitleMetaXml",
                             reinterpret_cast<void **>(&get_meta));
    }
    if (get_meta == nullptr) {
        return false;
    }
    // The call wants 64-byte alignment and the struct is too big for a comfortable stack.
    auto *meta = static_cast<ACPMetaXml *>(memalign(64, sizeof(ACPMetaXml)));
    if (meta == nullptr) {
        return false;
    }
    std::memset(meta, 0, sizeof(ACPMetaXml));
    report.meta = static_cast<uint32_t>(get_meta(OSGetTitleID(), meta));
    report.key = meta->olv_accesskey;
    free(meta);
    return report.meta == 0 && report.key != 0;
}

bool initialize() {
    if (initialized) {
        report.initialized_before = true;
        return true;
    }
    // One buffer for every try, so a retry after a failed Initialize doesn't take another.
    if (init_work == nullptr) {
        init_work = static_cast<uint8_t *>(memalign(32, WORK_BYTES));
    }
    if (init_work == nullptr) {
        return false;
    }
    std::memset(init_work, 0, WORK_BYTES);
    InitParam param = {};
    param.work = init_work;
    param.work_size = WORK_BYTES;
    // It blocks, for seconds the first time, which is why none of this is on the listener.
    const uint64_t start = now_ms();
    report.initialize = static_cast<uint32_t>(olv.initialize(&param));
    report.initialize_ms = static_cast<uint32_t>(now_ms() - start);
    initialized = report.initialize == OLV_OK || report.initialize == OLV_ROSE_OK ||
                  report.initialize == OLV_ALREADY;
    return initialized;
}

// Which community the library says is YouTube's, for checking the table above. Once per
// process, after Initialize and before the post, and never on its own: it's a download, so it
// costs the first share a round trip. Nothing depends on what it answers.
void probe_community() {
    if (probed) {
        report.probe = probe_code;
        report.probe_id = probe_id;
        return;
    }
    probed = true;
    if (!olv.probe_param || !olv.probe_max || !olv.probe_topic || !olv.probe_post ||
        !olv.probe_download || !olv.probe_community) {
        return;
    }
    auto *param = static_cast<uint8_t *>(memalign(64, PROBE_PARAM_BYTES));
    auto *topic = static_cast<uint8_t *>(memalign(64, PROBE_TOPIC_BYTES));
    auto *post = static_cast<uint8_t *>(memalign(64, PROBE_POST_BYTES));
    if (param != nullptr && topic != nullptr && post != nullptr) {
        std::memset(param, 0, PROBE_PARAM_BYTES);
        std::memset(topic, 0, PROBE_TOPIC_BYTES);
        std::memset(post, 0, PROBE_POST_BYTES);
        olv.probe_param(param);
        olv.probe_max(param, 1);
        olv.probe_topic(topic);
        olv.probe_post(post);
        // No community set, so the server answers with the title's own.
        uint32_t count = 0;
        probe_code = olv.probe_download(topic, post, &count, 1, param);
        if ((probe_code & 0x80000000u) == 0) {
            probe_id = olv.probe_community(topic);
        }
    }
    report.probe = probe_code;
    report.probe_id = probe_id;
    free(param);
    free(topic);
    free(post);
}

bool failed(uint32_t code) {
    return (code & 0x80000000u) != 0;
}

// The post itself. Every block here is only read until UploadPostDataByPostApp returns, which
// is the engine's reading too, so all of it goes straight after.
void post() {
    auto *param = static_cast<uint8_t *>(memalign(32, PARAM_BYTES));
    auto *work = static_cast<uint8_t *>(memalign(32, WORK_BYTES));
    // The body and the app data in one 64-byte aligned block, the app data first.
    auto *text = static_cast<uint8_t *>(memalign(64, 64 + sizeof(body)));
    if (param == nullptr || work == nullptr || text == nullptr) {
        free(param);
        free(work);
        free(text);
        finish("failed", "no memory for the post");
        return;
    }
    std::memset(param, 0, PARAM_BYTES);
    std::memset(work, 0, WORK_BYTES);
    std::memset(text, 0, 64 + sizeof(body));
    std::memcpy(text, video, VIDEO_ID_LENGTH);
    auto *units = reinterpret_cast<uint16_t *>(text + 64);
    std::memcpy(units, body, sizeof(body));

    const char *stopped = nullptr;
    olv.construct(param);
    report.work = olv.set_work(param, work, WORK_BYTES);
    if (failed(report.work)) {
        stopped = "SetWork";
    }
    if (stopped == nullptr) {
        report.set_community = olv.set_community(param, report.community);
        if (failed(report.set_community)) {
            stopped = "SetCommunityId";
        }
    }
    if (stopped == nullptr && olv.set_body_max != nullptr) {
        report.body_max = olv.set_body_max(param, TEXT_MAX);
        if (failed(report.body_max)) {
            stopped = "SetBodyTextMaxLength";
        }
    }
    if (stopped == nullptr && olv.set_body_max == nullptr && body_units > TEXT_DEFAULT_MAX) {
        stopped = "SetBodyTextMaxLength";
    }
    if (stopped == nullptr) {
        report.body = olv.set_body(param, units);
        if (failed(report.body)) {
            stopped = "SetBodyText";
        }
    }
    if (stopped == nullptr && olv.set_app_data != nullptr) {
        report.app_data = olv.set_app_data(param, text, VIDEO_ID_LENGTH);
        if (failed(report.app_data)) {
            stopped = "SetAppData";
        }
    }

    // A link the library turns down still leaves a post with the link in its text, so a
    // refusal here is reported and doesn't stop it. The long form, which both the archived Juxt
    // pages and Closedverse's pattern read; whether the revivals keep it is the console test.
    if (stopped == nullptr && olv.set_url != nullptr) {
        static char url[64];
        std::snprintf(url, sizeof(url), "https://www.youtube.com/watch?v=%s", video);
        report.url = olv.set_url(param, url);
    }

    if (stopped == nullptr) {
        // Only the switch request. The app gives up the foreground once it's taken, and what
        // came of the post is read when it comes back, in olv_share_foreground.
        report.upload = static_cast<uint32_t>(olv.upload(param));
    }
    free(param);
    free(work);
    free(text);

    if (stopped != nullptr) {
        finish("refused by the library", stopped);
        return;
    }
    if (report.upload != OLV_OK && report.upload != OLV_ROSE_OK) {
        finish("failed", "UploadPostDataByPostApp");
        return;
    }
    report.outcome = "post screen open";
    state.store(State::Launched);
}

// Refused for the rest of the process: the library points somewhere a share mustn't post.
void refuse_library(const char *why) {
    unusable = true;
    finish("refused", why);
}

int run(int, const char **) {
    if (library == nullptr) {
        acquire_tried = true;
        report.acquire = static_cast<uint32_t>(OSDynLoad_Acquire("nn_olv.rpl", &library));
        if (report.acquire != OS_DYNLOAD_OK) {
            library = nullptr;
            finish("failed", "nn_olv.rpl didn't load");
            return 0;
        }
    } else {
        report.acquire = OS_DYNLOAD_OK;
    }

    // The acquire is the late load olv.cpp's watch points at the selected Miiverse, so by now
    // the library should name exactly one host and it should be that one.
    bool listed = false;
    const uint32_t hosts = olv_scan_hosts(&listed);
    olv_hosts_label(hosts, report.hosts, sizeof(report.hosts));
    if (!listed) {
        finish("failed", "the loader doesn't list nn_olv");
        return 0;
    }
    if (hosts & OLV_HOST_NINTENDO) {
        refuse_library("Nintendo's host is still in nn_olv");
        return 0;
    }
    if (hosts == 0 || (hosts & (hosts - 1)) != 0) {
        refuse_library(hosts == 0 ? "no host in nn_olv" : "more than one host in nn_olv");
        return 0;
    }
    if ((hosts & olv_hosts_of(Config::miiverse)) == 0) {
        refuse_library("nn_olv points at another Miiverse");
        return 0;
    }

    if (!resolved) {
        const char *missing = resolve();
        if (missing != nullptr) {
            finish("failed", missing);
            return 0;
        }
        resolved = true;
    }
    if (!start_services()) {
        finish("failed", "the title service didn't start");
        return 0;
    }
    if (!read_key()) {
        finish("refused", "no Miiverse access key in YouTube's meta");
        return 0;
    }
    if (!initialize()) {
        finish("failed", "Initialize");
        return 0;
    }
    probe_community();
    post();
    return 0;
}

bool start_thread() {
    if (stack == nullptr) {
        stack = static_cast<uint8_t *>(memalign(16, STACK_BYTES));
    }
    if (stack == nullptr) {
        return false;
    }
    if (!OSCreateThread(&thread, run, 0, nullptr, stack + STACK_BYTES, STACK_BYTES,
                        THREAD_PRIORITY, OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        return false;
    }
    // Detached, so it's gone the moment it returns and its OSThread can take the next share.
    // The stack stays for the process and is reused.
    OSDetachThread(&thread);
    OSSetThreadName(&thread, "JustGetMiiOnline share");
    thread_made = true;
    OSResumeThread(&thread);
    return true;
}

} // namespace

bool olv_share_available() {
    if (unusable || community_of(Config::miiverse) == 0) {
        return false;
    }
    // Protaverse can only be posted to where the late load can be pointed at it.
    return olv_late_target_known();
}

int olv_share_start(const char *video_id, const char *title, const char *channel) {
    if (!video_id_ok(video_id) || title == nullptr || channel == nullptr) {
        return 400;
    }
    if (!olv_share_available()) {
        return 503;
    }

    State expected = State::Idle;
    if (!state.compare_exchange_strong(expected, State::Running)) {
        expected = State::Done;
        if (!state.compare_exchange_strong(expected, State::Running)) {
            return 409;
        }
    }
    if (finished_once && now_ms() - finished_ms < GAP_MS) {
        state.store(State::Done);
        return 409;
    }
    // A thread from an earlier share in this process that hasn't quite returned yet.
    if (thread_made && !OSIsThreadTerminated(&thread)) {
        state.store(State::Done);
        return 409;
    }

    failed_ms.store(0);
    std::memcpy(video, video_id, VIDEO_ID_LENGTH);
    video[VIDEO_ID_LENGTH] = '\0';
    if (!build_body(title, channel, video)) {
        state.store(State::Done);
        return 400;
    }

    report = {};
    report.ran = true;
    std::memcpy(report.video, video, sizeof(report.video));
    report.units = body_units;
    report.community = community_of(Config::miiverse);
    report.outcome = "running";
    report.acquire = NOT_RUN;
    report.meta = NOT_RUN;
    report.initialize = NOT_RUN;
    report.probe = NOT_RUN;
    report.probe_id = NOT_RUN;
    report.work = NOT_RUN;
    report.set_community = NOT_RUN;
    report.body_max = NOT_RUN;
    report.body = NOT_RUN;
    report.app_data = NOT_RUN;
    report.url = NOT_RUN;
    report.upload = NOT_RUN;
    report.result = NOT_RUN;
    std::snprintf(report.hosts, sizeof(report.hosts), "not scanned");

    if (!start_thread()) {
        finish("failed", "no thread");
        return 503;
    }
    LOG("share: started for %s, %u units", video, static_cast<unsigned>(body_units));
    return 202;
}

void olv_share_preload() {
    if (!olv_share_available() || library != nullptr) {
        return;
    }
    preload_tried = true;
    const OSTime start = OSGetTime();
    LOG("share: nn_olv preload starting");
    preload_code = static_cast<uint32_t>(OSDynLoad_Acquire("nn_olv.rpl", &library));
    if (preload_code != OS_DYNLOAD_OK) {
        library = nullptr;
    }
    LOG("share: nn_olv preload %08X in %u ms", static_cast<unsigned>(preload_code),
        static_cast<unsigned>(OSTicksToMilliseconds(OSGetTime() - start)));
}

void olv_share_reset() {
    failed_ms.store(0);
    preload_tried = false;
    preload_code = 0;
    acquire_tried = false;
    state.store(State::Idle);
    finished_ms = 0;
    finished_once = false;
    unusable = false;
    library = nullptr;
    resolved = false;
    olv = {};
    services = false;
    initialized = false;
    probed = false;
    probe_code = NOT_RUN;
    probe_id = NOT_RUN;
    init_work = nullptr;
    stack = nullptr;
    thread_made = false;
}

int olv_share_state() {
    switch (state.load()) {
        case State::Running: return SHARE_STATE_OPENING;
        case State::Launched: return SHARE_STATE_OPEN;
        default: break;
    }
    const uint32_t failed = failed_ms.load();
    if (failed != 0 && static_cast<uint32_t>(now_ms()) - failed < FAILED_HOLD_MS) {
        return SHARE_STATE_FAILED;
    }
    return SHARE_STATE_IDLE;
}

void olv_share_foreground() {
    State expected = State::Launched;
    if (!state.compare_exchange_strong(expected, State::Running)) {
        return;
    }
    if (olv.get_result == nullptr) {
        finish_after_screen("back, GetResultByPostApp not exported");
        return;
    }
    report.result = olv.get_result();
    switch (report.result) {
        case OLV_OK: finish_after_screen("posted"); return;
        case OLV_CANCEL_OR_FAILURE: finish_after_screen("canceled or failed"); return;
        case OLV_NOTHING_BACK: finish_after_screen("nothing came back"); return;
    }
    finish_after_screen("back with an unknown result");
}

namespace {

// A code for the readout, or "not run" for a step the share never reached.
const char *code(char *out, size_t size, uint32_t value) {
    if (value == NOT_RUN) {
        std::snprintf(out, size, "not run");
    } else {
        std::snprintf(out, size, "%08X", static_cast<unsigned>(value));
    }
    return out;
}

} // namespace

// Eight lines: what came of it, where the library pointed, the title's key, Initialize, the
// community probe, the setters and the upload with its result. Codes are the library's own
// answers in hex.
bool olv_share_report_line(size_t index, char *out, size_t size) {
    if (!report.ran) {
        return false;
    }
    char a[12];
    char b[12];
    char c[12];
    switch (index) {
        case 0:
            std::snprintf(out, size, "Share: %s%s%s", report.outcome ? report.outcome : "",
                          report.why ? ", " : "", report.why ? report.why : "");
            return true;
        case 1:
            if (preload_tried) {
                std::snprintf(b, sizeof(b), "%08X", static_cast<unsigned>(preload_code));
            } else {
                std::snprintf(b, sizeof(b), "not run");
            }
            if (acquire_tried) {
                std::snprintf(a, sizeof(a), "%08X", static_cast<unsigned>(report.acquire));
            } else {
                code(a, sizeof(a), report.acquire);
            }
            std::snprintf(out, size, "Share nn_olv: preload %s, acquire %s, host %s", b, a,
                          report.hosts);
            return true;
        case 2:
            std::snprintf(out, size, "Share key %08X, meta %s, title patch skipped",
                          static_cast<unsigned>(report.key), code(a, sizeof(a), report.meta));
            return true;
        case 3:
            if (report.initialized_before) {
                std::snprintf(out, size, "Share Initialize: done earlier in this process");
            } else {
                std::snprintf(out, size, "Share Initialize %s in %u ms",
                              code(a, sizeof(a), report.initialize),
                              static_cast<unsigned>(report.initialize_ms));
            }
            return true;
        case 4:
            if (report.probe_id == NOT_RUN) {
                std::snprintf(out, size, "Share community %u, YouTube's unread (probe %s)",
                              static_cast<unsigned>(report.community),
                              code(a, sizeof(a), report.probe));
            } else {
                std::snprintf(out, size, "Share community %u, YouTube's %u (probe %s)",
                              static_cast<unsigned>(report.community),
                              static_cast<unsigned>(report.probe_id),
                              code(a, sizeof(a), report.probe));
            }
            return true;
        case 5:
            std::snprintf(out, size, "Share work %s, community %s, max %s",
                          code(a, sizeof(a), report.work), code(b, sizeof(b), report.set_community),
                          code(c, sizeof(c), report.body_max));
            return true;
        case 6:
            std::snprintf(out, size, "Share text %s (%u units), app data %s, link %s",
                          code(a, sizeof(a), report.body), static_cast<unsigned>(report.units),
                          code(b, sizeof(b), report.app_data), code(c, sizeof(c), report.url));
            return true;
        case 7:
            std::snprintf(out, size, "Share upload %s, GetResult %s",
                          code(a, sizeof(a), report.upload), code(b, sizeof(b), report.result));
            return true;
    }
    return false;
}
