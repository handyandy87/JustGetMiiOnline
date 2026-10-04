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

#include "spotpass.h"
#include "iosu.h"
#include "logger.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

// The two tasksheet slots. Same URL in two shapes, differing by one path segment,
// and both builds aim both of them at a single host.
constexpr uint32_t TASKSHEET_LONG_ADDRESS = 0xE229B1F4;
constexpr uint32_t TASKSHEET_SHORT_ADDRESS = 0xE229B238;

// The other two addresses SpotPass also occupies: a policy list and the policy
// hostname.
//
// Protarium's older module points only the tasksheets at itself and leaves this pair
// on Pretendo. That's why moving SpotPass to Pretendo there has only the tasksheets to
// move: the other half already holds what would be written, and the check that comes
// before any write says so and writes nothing.
//
// Their newer module points this pair at itself as well, so there Pretendo moves all
// four, the way Roseverse always has on both.
constexpr uint32_t BOSS_POLICY_HOST_ADDRESS = 0xE2299990;
constexpr uint32_t BOSS_POLICY_LIST_ADDRESS = 0xE24B8A24;

// Writing this makes the BOSS policy state reload after a URL change. Without it
// the console goes on using the cached old address.
constexpr uint32_t BOSS_POLICY_REFRESH_ADDRESS = 0xE24B3D90;
constexpr uint32_t BOSS_POLICY_REFRESH_VALUE = 4;

// Pretendo's tasksheet URLs.
constexpr char PRETENDO_LONG_URL[] =
    "https://npts.app.pretendo.cc/p01/tasksheet/%s/%s/%s/%s?c=%s&l=%s";
constexpr char PRETENDO_SHORT_URL[] =
    "https://npts.app.pretendo.cc/p01/tasksheet/%s/%s/%s?c=%s&l=%s";
static_assert(sizeof(PRETENDO_LONG_URL) == 65);
static_assert(sizeof(PRETENDO_SHORT_URL) == 62);

// Rose's. Same lengths as Pretendo's, because nts.projectrose.cafe and
// npts.app.pretendo.cc are both twenty characters, so the whole room argument below
// carries over without a word of it changing. The assertions say that instead of the
// numbers saying it.
constexpr char ROSEVERSE_LONG_URL[] =
    "https://nts.projectrose.cafe/p01/tasksheet/%s/%s/%s/%s?c=%s&l=%s";
constexpr char ROSEVERSE_SHORT_URL[] =
    "https://nts.projectrose.cafe/p01/tasksheet/%s/%s/%s?c=%s&l=%s";
static_assert(sizeof(ROSEVERSE_LONG_URL) == sizeof(PRETENDO_LONG_URL));
static_assert(sizeof(ROSEVERSE_SHORT_URL) == sizeof(PRETENDO_SHORT_URL));

// The policy pair. On the older module the incumbent is Pretendo's own string, and the
// replacement matches its length to the byte. On the newer one the incumbent is
// Protarium's, two bytes longer, because nppl.app.protarium.lol is two characters
// longer than nppl.app.pretendo.cc.
//
// That puts these two in a stronger spot than the tasksheets rather than a weaker one,
// which is worth being clear about because they're the two addresses nothing here has
// ever read on a console. There's no room argument to get wrong: nothing is being
// fitted into space left by a longer string, only into the incumbent itself or less of
// it. Matching the whole incumbent is simply the only proof available that the address
// is right, and it's the same technique and the same reason as olv.cpp's.
constexpr char PRETENDO_POLICY_HOST[] = "nppl.app.pretendo.cc";
constexpr char ROSEVERSE_POLICY_HOST[] = "npl.projectrose.cafe";
constexpr char PROTARIUM_POLICY_HOST[] = "nppl.app.protarium.lol";
static_assert(sizeof(PRETENDO_POLICY_HOST) == 21);
static_assert(sizeof(ROSEVERSE_POLICY_HOST) == sizeof(PRETENDO_POLICY_HOST));
static_assert(sizeof(PROTARIUM_POLICY_HOST) == 23);

constexpr char PRETENDO_POLICY_LIST[] = "https://nppl.app.pretendo.cc/p01/policylist/1/1/UNK";
constexpr char ROSEVERSE_POLICY_LIST[] = "https://npl.projectrose.cafe/p01/policylist/1/1/UNK";
constexpr char PROTARIUM_POLICY_LIST[] =
    "https://nppl.app.protarium.lol/p01/policylist/1/1/UNK";
static_assert(sizeof(PRETENDO_POLICY_LIST) == 52);
static_assert(sizeof(ROSEVERSE_POLICY_LIST) == sizeof(PRETENDO_POLICY_LIST));
static_assert(sizeof(PROTARIUM_POLICY_LIST) == 54);

// The whole slot, not just the string in it. This is the part worth reading twice.
//
// On Protarium's older module, Pretendo's URLs are three bytes longer than Protarium's,
// because npts.app.pretendo.cc is longer than api.protarium.lol. So measuring the room by
// walking to the first terminator, which is what account.cpp does for the account URL,
// measures Protarium's 62 bytes rather than the slot, and refuses a write that's in
// fact safe. That measurement is right for the account URL only because Pretendo's
// string happens to be shorter there.
//
// So the slot gets proved the way olv.cpp proves the Miiverse one. Nintendo's
// originals are npts.app.nintendo.net, one byte longer again, which makes the slots 66
// and 63 bytes. Protarium's shorter strings don't erase the tail of what they
// replaced: the bytes past the terminator survive. Both slots therefore end "=%s" and
// a terminator, stranded from Nintendo's own "&l=%s".
//
// Matching all 66 bytes does three jobs at once that would otherwise be three separate
// guesses. It proves the address is right on this console, far more strongly than
// checking for "https://". It measures the slot, because a string plus a stranded tail
// can't be that long unless the slot is. And it keeps the house rule of never writing
// more bytes than were matched, with room to spare, since what goes back is one byte
// shorter than what was matched.
constexpr char API_LONG_SLOT[66] =
    "https://api.protarium.lol/p01/tasksheet/%s/%s/%s/%s?c=%s&l=%s\0" "=%s";
constexpr char API_SHORT_SLOT[63] =
    "https://api.protarium.lol/p01/tasksheet/%s/%s/%s?c=%s&l=%s\0" "=%s";
static_assert(sizeof(API_LONG_SLOT) == 66);
static_assert(sizeof(API_SHORT_SLOT) == 63);

// How much of each slot is Protarium's own string, terminator included. Used to
// tell "this is not their slot at all" from "their string is here and the tail is
// not what was expected", which are different faults and want different answers.
constexpr size_t API_LONG_LENGTH = 62;
constexpr size_t API_SHORT_LENGTH = 59;
static_assert(API_LONG_LENGTH < sizeof(API_LONG_SLOT));
static_assert(API_SHORT_LENGTH < sizeof(API_SHORT_SLOT));

// The newer module's tasksheets, on npts.app.protarium.lol, which is one character
// longer than Nintendo's npts.app.nintendo.net. So this string covers the whole of
// Nintendo's, terminator included, and leaves no stranded tail to prove the slot any
// wider than the string itself. The width matched is the string and nothing more.
// That's still enough for the house rule, since Pretendo's and Rose's URLs are two
// bytes shorter than it.
constexpr char NPTS_LONG_SLOT[] =
    "https://npts.app.protarium.lol/p01/tasksheet/%s/%s/%s/%s?c=%s&l=%s";
constexpr char NPTS_SHORT_SLOT[] =
    "https://npts.app.protarium.lol/p01/tasksheet/%s/%s/%s?c=%s&l=%s";
static_assert(sizeof(NPTS_LONG_SLOT) == 67);
static_assert(sizeof(NPTS_SHORT_SLOT) == 64);

// Never more bytes written than were matched, on either module. Rose's are the same
// lengths as Pretendo's, asserted above, so these cover hers too.
static_assert(sizeof(PRETENDO_LONG_URL) <= sizeof(API_LONG_SLOT));
static_assert(sizeof(PRETENDO_SHORT_URL) <= sizeof(API_SHORT_SLOT));
static_assert(sizeof(PRETENDO_LONG_URL) <= sizeof(NPTS_LONG_SLOT));
static_assert(sizeof(PRETENDO_SHORT_URL) <= sizeof(NPTS_SHORT_SLOT));
static_assert(sizeof(PRETENDO_POLICY_LIST) <= sizeof(PROTARIUM_POLICY_LIST));
static_assert(sizeof(PRETENDO_POLICY_HOST) <= sizeof(PROTARIUM_POLICY_HOST));

// The widest slot either module leaves, which every read below is sized to.
constexpr size_t WIDEST_SLOT = sizeof(NPTS_LONG_SLOT);
static_assert(sizeof(API_LONG_SLOT) <= WIDEST_SLOT);
static_assert(sizeof(API_SHORT_SLOT) <= WIDEST_SLOT);
static_assert(sizeof(NPTS_SHORT_SLOT) <= WIDEST_SLOT);
static_assert(sizeof(PROTARIUM_POLICY_LIST) <= WIDEST_SLOT);
static_assert(sizeof(PROTARIUM_POLICY_HOST) <= WIDEST_SLOT);

// Which of Protarium's two modules this console has, named by where each one points
// the tasksheets.
//
// Api is the older: tasksheets on api.protarium.lol, the policy pair left on Pretendo.
// Npts is the newer: tasksheets on npts.app.protarium.lol and the policy pair on
// nppl.app.protarium.lol. Both write the same addresses in the same order and differ in
// exactly these four strings, so the long tasksheet slot alone tells them apart, and
// the other three are judged against whichever one it shows. The plugin can't see
// either module's version string, so this is the only way it has of knowing.
enum class Layout {
    NotRead,
    NotRecognized,
    Api,
    Npts,
};

// Where SpotPass is being sent. Which network that is comes from the setting now
// rather than from a rule here. What's left here is what each answer costs in
// addresses, which is the part that belongs beside them.
//
// Protarium is LeaveAlone rather than a set of writes because their build already
// points all four of these where it wants them. That is what keeps this position
// indistinguishable from the plugin not being installed.
enum class Destination {
    LeaveAlone,
    Pretendo,
    Roseverse,
};

constexpr Destination destination_for(SpotPass chosen) {
    switch (chosen) {
        case SpotPass::Pretendo: return Destination::Pretendo;
        case SpotPass::Roseverse: return Destination::Roseverse;
        case SpotPass::Protarium: break;
    }
    return Destination::LeaveAlone;
}

// The hosts each of these addresses can legitimately hold, for reporting what is
// actually there. Spelled out separately from the URLs above rather than parsed out of
// them: a parser would be three more things to get wrong in a readout whose whole job
// is to be trusted over the setting.
constexpr const char *TASKSHEET_HOST_API = "api.protarium.lol";
constexpr const char *TASKSHEET_HOST_NPTS = "npts.app.protarium.lol";
constexpr const char *TASKSHEET_HOST_PRETENDO = "npts.app.pretendo.cc";
constexpr const char *TASKSHEET_HOST_ROSEVERSE = "nts.projectrose.cafe";
constexpr const char *POLICY_HOST_PROTARIUM = "nppl.app.protarium.lol";
constexpr const char *POLICY_HOST_PRETENDO = "nppl.app.pretendo.cc";
constexpr const char *POLICY_HOST_ROSEVERSE = "npl.projectrose.cafe";

// What one of Protarium's modules leaves at one address.
struct Incumbent {
    // The whole slot as that module leaves it. For the older module's tasksheets that is
    // Protarium's string plus the tail stranded from Nintendo's; everywhere else it's
    // just the string.
    const char *bytes;
    size_t slot_width;
    // Just the incumbent string, terminator included. Equal to slot_width where there
    // is no stranded tail to argue from, which is what makes the tail branch
    // unreachable everywhere but the older module's tasksheets instead of specially
    // disabled for the rest.
    size_t length;
    // Reporting only: the host that string names.
    const char *host;
};

struct Slot {
    uint32_t address;
    const char *label;
    Incumbent api;
    Incumbent npts;
    const char *pretendo;
    size_t pretendo_length;
    const char *roseverse;
    size_t roseverse_length;
    // Reporting only. The policy pair is hidden from the menu on every combination
    // that cannot move it, and this is how a slot says which pair it belongs to
    // without the menu having to know two addresses.
    bool policy;
    char *status;
    size_t status_size;
    // Reporting only, and the other half of the story: the hosts this address can hold
    // beyond the two modules' own, and where the answer goes.
    const char *pretendo_host;
    const char *roseverse_host;
    char *seen;
    size_t seen_size;
};

char long_status[64] = "left alone";
char short_status[64] = "left alone";
char policy_status[64] = "left alone";
char policy_host_status[64] = "left alone";

// Deliberately not "left alone": nothing has been read yet, which is a different
// statement from having read something and found the module's own value.
char long_seen[40] = "not read yet";
char short_seen[40] = "not read yet";
char policy_seen[40] = "not read yet";
char policy_host_seen[40] = "not read yet";

// Set once either policy slot has a result worth showing, so a console that moved them
// and then had the setting changed under it still shows what's actually in them
// instead of hiding the lines because the new setting wouldn't move them.
bool policy_reported = false;

// What this boot has already sent SpotPass to, and whether it sent it anywhere.
//
// Recorded rather than read back out of the slots, and that distinction is the whole
// point. Inferring it from memory would mean treating any string this plugin happens
// to recognize as its own earlier write, which is exactly the mistake the account and
// build checks were fixed for: on a console running a different Inkay build, that
// build's writes would get reported as this one's. A value this file set itself cannot
// be anybody else's.
Destination applied = Destination::LeaveAlone;
bool have_applied = false;

// Which module the writes this boot were judged against, once the long slot proved
// one. Recorded for the same reason as the two above: after this plugin's own write
// the slot no longer holds the module's string, and the next application start has to
// keep judging the other slots against the module the first one proved.
Layout layout = Layout::NotRead;

// What the menu's own read of the long slot showed, for the Debug page.
Layout layout_seen = Layout::NotRead;

const Incumbent &incumbent_for(const Slot &slot, Layout which) {
    return which == Layout::Npts ? slot.npts : slot.api;
}

// The most either module leaves at this address, which is what a read that has to
// cover both of them asks for.
size_t widest(const Slot &slot) {
    return slot.api.slot_width > slot.npts.slot_width ? slot.api.slot_width
                                                       : slot.npts.slot_width;
}

// Which module the whole of this slot matches, compared at full width. Nothing short of
// every byte counts: a prefix match is what "their string with an unexpected tail"
// means, and that is a fault to report, not a module to act on.
Layout layout_of(const Slot &slot, const unsigned char *bytes) {
    if (std::memcmp(bytes, slot.api.bytes, slot.api.slot_width) == 0) {
        return Layout::Api;
    }
    if (std::memcmp(bytes, slot.npts.bytes, slot.npts.slot_width) == 0) {
        return Layout::Npts;
    }
    return Layout::NotRecognized;
}

// Whether this slot holds one of this plugin's own strings written over that module's,
// with every byte past the write still the module's. That's still a proof of which
// module it is, because the two leave different bytes there: the older one the last
// byte of Nintendo's stranded tail, the newer one the end of its own longer string.
bool written_over(const Slot &slot, const Incumbent &incumbent, const unsigned char *bytes) {
    const char *const strings[] = {slot.pretendo, slot.roseverse};
    const size_t lengths[] = {slot.pretendo_length, slot.roseverse_length};
    for (size_t i = 0; i < 2; ++i) {
        if (lengths[i] <= incumbent.slot_width &&
            std::memcmp(bytes, strings[i], lengths[i]) == 0 &&
            std::memcmp(bytes + lengths[i], incumbent.bytes + lengths[i],
                        incumbent.slot_width - lengths[i]) == 0) {
            return true;
        }
    }
    return false;
}

const char *layout_name(Layout which) {
    switch (which) {
        case Layout::Api: return "older";
        case Layout::Npts: return "newer";
        case Layout::NotRecognized: return "not recognized";
        case Layout::NotRead: break;
    }
    return "not read yet";
}

// Report the tail rather than a verdict.
//
// This is the one assumption in the file that nothing in the repo can settle, so when
// it fails the console says what it actually found instead of just saying no. If this
// ever shows up in the menu, the bytes in it are the whole answer: the fix is to accept
// whatever they are.
// Takes the bytes the caller already read rather than reading again, so what it
// reports is the same snapshot the decision was made on.
void report_tail(const Slot &slot, const Incumbent &incumbent,
                 const unsigned char *slot_bytes) {
    const unsigned char *tail = slot_bytes + incumbent.slot_width - 4;
    std::snprintf(slot.status, slot.status_size, "%s: tail %02x %02x %02x %02x", slot.label,
                  tail[0], tail[1], tail[2], tail[3]);
    LOG("%s slot at %08x holds their string but tail %02x %02x %02x %02x, leaving it alone",
        slot.label, slot.address, tail[0], tail[1], tail[2], tail[3]);
}

// What this slot should end up holding, or nothing when this destination has
// nothing to move here. Every slot has a Pretendo and a Rose string now, so the only
// destination that returns null is LeaveAlone, which never gets this far.
const char *wanted(const Slot &slot, Destination destination, size_t &length) {
    switch (destination) {
        case Destination::Pretendo:
            length = slot.pretendo_length;
            return slot.pretendo;
        case Destination::Roseverse:
            length = slot.roseverse_length;
            return slot.roseverse;
        case Destination::LeaveAlone:
            break;
    }
    length = 0;
    return nullptr;
}

const char *destination_name(Destination destination) {
    switch (destination) {
        case Destination::Pretendo: return "Pretendo";
        case Destination::Roseverse: return "Roseverse";
        case Destination::LeaveAlone: break;
    }
    return "left alone";
}

// True when this call changed something, which is what decides whether the policy
// state has to be told to reload.
bool move_slot(const IosuHandle &iosu, const Slot &slot, const Incumbent &incumbent,
               Destination destination) {
    size_t length = 0;
    const char *replacement = wanted(slot, destination, length);
    if (replacement == nullptr) {
        return false;
    }

    // One read, then plain comparisons against it, rather than a fresh read per
    // candidate. Two reasons, and the second is the one hardware found.
    //
    // A read that fails isn't a statement about the bytes. On a cold boot these four
    // addresses can't be read at all for a while, while the account URL that a
    // different IOSU module owns reads perfectly well in the same boot; after a reload
    // of the menu and plugins they read fine and report Protarium. So a failed read
    // here means "not yet", and reporting it as "not theirs" would accuse the console
    // of running a build it's not running, on every cold boot. Nothing is written,
    // nothing is concluded, and the next application start tries again, in whichever
    // title that happens to be.
    unsigned char now[WIDEST_SLOT] = {};
    if (incumbent.slot_width > sizeof(now) ||
        !iosu.read_bytes(slot.address, now, incumbent.slot_width)) {
        std::snprintf(slot.status, slot.status_size, "%s: not read yet", slot.label);
        if (slot.policy) {
            policy_reported = true;
        }
        return false;
    }

    // Asked first, so a second application start does no work and the menu still
    // reads as done rather than as refused.
    if (std::memcmp(now, replacement, length) == 0) {
        std::snprintf(slot.status, slot.status_size, "%s: %s", slot.label,
                      destination_name(destination));
        if (slot.policy) {
            policy_reported = true;
        }
        return false;
    }

    if (std::memcmp(now, incumbent.bytes, incumbent.slot_width) != 0) {
        // Split the two reasons apart. The incumbent being absent means this is not
        // the slot this thinks it is, and the only safe move is to leave it alone. The
        // incumbent being present with an unexpected tail means the address is right
        // and only the room argument failed. Everywhere but the older module's
        // tasksheets the two widths are equal, so the second branch can't fire and a
        // mismatch falls straight through to "not theirs" with no new vocabulary to
        // learn.
        if (incumbent.length < incumbent.slot_width &&
            std::memcmp(now, incumbent.bytes, incumbent.length) == 0) {
            report_tail(slot, incumbent, now);
        } else {
            std::snprintf(slot.status, slot.status_size, "%s: not theirs", slot.label);
            LOG("%08x does not hold the expected %s, leaving it alone", slot.address,
                slot.label);
        }
        if (slot.policy) {
            policy_reported = true;
        }
        return false;
    }

    if (!iosu.write_bytes(slot.address, replacement, length)) {
        std::snprintf(slot.status, slot.status_size, "%s: write failed", slot.label);
        if (slot.policy) {
            policy_reported = true;
        }
        return false;
    }

    std::snprintf(slot.status, slot.status_size, "%s: %s", slot.label,
                  destination_name(destination));
    if (slot.policy) {
        policy_reported = true;
    }
    LOG("%s slot pointed at %s", slot.label, destination_name(destination));
    return true;
}

const Slot SLOTS[] = {
    {TASKSHEET_LONG_ADDRESS, "long",
     {API_LONG_SLOT, sizeof(API_LONG_SLOT), API_LONG_LENGTH, TASKSHEET_HOST_API},
     {NPTS_LONG_SLOT, sizeof(NPTS_LONG_SLOT), sizeof(NPTS_LONG_SLOT), TASKSHEET_HOST_NPTS},
     PRETENDO_LONG_URL, sizeof(PRETENDO_LONG_URL), ROSEVERSE_LONG_URL,
     sizeof(ROSEVERSE_LONG_URL), false, long_status, sizeof(long_status),
     TASKSHEET_HOST_PRETENDO, TASKSHEET_HOST_ROSEVERSE, long_seen, sizeof(long_seen)},
    {TASKSHEET_SHORT_ADDRESS, "short",
     {API_SHORT_SLOT, sizeof(API_SHORT_SLOT), API_SHORT_LENGTH, TASKSHEET_HOST_API},
     {NPTS_SHORT_SLOT, sizeof(NPTS_SHORT_SLOT), sizeof(NPTS_SHORT_SLOT), TASKSHEET_HOST_NPTS},
     PRETENDO_SHORT_URL, sizeof(PRETENDO_SHORT_URL), ROSEVERSE_SHORT_URL,
     sizeof(ROSEVERSE_SHORT_URL), false, short_status, sizeof(short_status),
     TASKSHEET_HOST_PRETENDO, TASKSHEET_HOST_ROSEVERSE, short_seen, sizeof(short_seen)},
    // On the older module the incumbent here is Pretendo's own string, the same one
    // Pretendo would write, so concerns() below keeps Pretendo away from this pair there
    // exactly as when it had no Pretendo string at all. There's no stranded tail on
    // either module and no room argument to make, just an exact match of the whole
    // incumbent.
    {BOSS_POLICY_LIST_ADDRESS, "policy",
     {PRETENDO_POLICY_LIST, sizeof(PRETENDO_POLICY_LIST), sizeof(PRETENDO_POLICY_LIST),
      POLICY_HOST_PRETENDO},
     {PROTARIUM_POLICY_LIST, sizeof(PROTARIUM_POLICY_LIST), sizeof(PROTARIUM_POLICY_LIST),
      POLICY_HOST_PROTARIUM},
     PRETENDO_POLICY_LIST, sizeof(PRETENDO_POLICY_LIST), ROSEVERSE_POLICY_LIST,
     sizeof(ROSEVERSE_POLICY_LIST), true, policy_status, sizeof(policy_status),
     POLICY_HOST_PRETENDO, POLICY_HOST_ROSEVERSE, policy_seen, sizeof(policy_seen)},
    {BOSS_POLICY_HOST_ADDRESS, "policy host",
     {PRETENDO_POLICY_HOST, sizeof(PRETENDO_POLICY_HOST), sizeof(PRETENDO_POLICY_HOST),
      POLICY_HOST_PRETENDO},
     {PROTARIUM_POLICY_HOST, sizeof(PROTARIUM_POLICY_HOST), sizeof(PROTARIUM_POLICY_HOST),
      POLICY_HOST_PROTARIUM},
     PRETENDO_POLICY_HOST, sizeof(PRETENDO_POLICY_HOST), ROSEVERSE_POLICY_HOST,
     sizeof(ROSEVERSE_POLICY_HOST), true, policy_host_status, sizeof(policy_host_status),
     POLICY_HOST_PRETENDO, POLICY_HOST_ROSEVERSE, policy_host_seen,
     sizeof(policy_host_seen)},
};

// The slot that says which module this is.
const Slot &LONG_SLOT = SLOTS[0];

// Whether this destination has anything to move or report at this slot.
//
// Pretendo and the policy pair are the one pairing where that depends on the module. On
// the older one the pair already holds Pretendo's strings, so there's nothing to move,
// and the pair stays "left alone" and off the menu the way it always has there. Only
// once the long slot has proved the newer module does Pretendo have any business with
// it. Until then the module is unknown, so it's treated the same way.
bool concerns(const Slot &slot, Destination destination) {
    return !(slot.policy && destination == Destination::Pretendo && layout != Layout::Npts);
}

void report(const Slot &slot, const char *what) {
    std::snprintf(slot.status, slot.status_size, "%s: %s", slot.label, what);
    if (slot.policy) {
        policy_reported = true;
    }
}

// Every slot this destination concerns, for the outcomes that apply to all of them.
void report_all(const char *what, Destination destination) {
    for (const Slot &slot : SLOTS) {
        if (concerns(slot, destination)) {
            report(slot, what);
        }
    }
}

// What the slots hold when the long slot proves no module, reported and never written.
//
// A plugin reloaded mid-session starts over with this boot's records gone, while its
// own earlier writes are still in the slots. Those slots say they're on the destination,
// as they did before any module had to be proved, and the rest keep "no layout".
void report_already_moved(const IosuHandle &iosu, Destination destination) {
    for (const Slot &slot : SLOTS) {
        size_t length = 0;
        const char *replacement = wanted(slot, destination, length);
        unsigned char now[WIDEST_SLOT] = {};
        if (concerns(slot, destination) && replacement != nullptr && length <= sizeof(now) &&
            iosu.read_bytes(slot.address, now, length) &&
            std::memcmp(now, replacement, length) == 0) {
            report(slot, destination_name(destination));
        }
    }
}

// Read the long slot and say which module it proves, for the writes to be judged
// against. Only the module's own string, matched at full width, counts here. This
// plugin's write over it is something the menu can recognize later in the boot, but it
// isn't something to start from. Pretendo's URL written over Nintendo's original by any
// other build leaves the same 66 bytes this plugin's Pretendo write over the older
// module does, because the byte after it is Nintendo's terminator either way.
//
// When it proves neither, nothing is written anywhere and every slot it concerns says
// so. The long slot keeps the one distinction worth keeping: the older module's string
// with an unexpected tail is the room argument failing, reported as bytes the way
// move_slot reports it.
Layout recognize(const IosuHandle &iosu, Destination destination) {
    unsigned char bytes[WIDEST_SLOT] = {};
    const size_t width = widest(LONG_SLOT);
    if (width > sizeof(bytes) || !iosu.read_bytes(LONG_SLOT.address, bytes, width)) {
        report_all("not read yet", destination);
        return Layout::NotRead;
    }

    const Layout found = layout_of(LONG_SLOT, bytes);
    if (found != Layout::NotRecognized) {
        LOG("SpotPass slots are the %s module's", layout_name(found));
        return found;
    }

    report_all("no layout", destination);
    for (const Incumbent *incumbent : {&LONG_SLOT.api, &LONG_SLOT.npts}) {
        if (incumbent->length < incumbent->slot_width &&
            std::memcmp(bytes, incumbent->bytes, incumbent->length) == 0) {
            report_tail(LONG_SLOT, *incumbent, bytes);
        }
    }
    LOG("%08x holds neither module's tasksheet, leaving all four alone",
        LONG_SLOT.address);
    return Layout::NotRecognized;
}

// Which module the long slot shows to the menu's read of it.
//
// The module's own string at full width says which one it is outright. This plugin's
// string over the front of it says the same, but only over the module this boot already
// proved, so the line never names a module that the slot's bytes don't show and that
// this boot didn't see.
Layout layout_shown(const Slot &slot, const unsigned char *bytes) {
    const Layout found = layout_of(slot, bytes);
    if (found != Layout::NotRecognized) {
        return found;
    }
    if ((layout == Layout::Api || layout == Layout::Npts) &&
        written_over(slot, incumbent_for(slot, layout), bytes)) {
        return layout;
    }
    return Layout::NotRecognized;
}

// What this address holds right now, named by host.
//
// Rose's is tested first and the modules' last, which only matters for the policy
// pair: there the older module's incumbent is Pretendo's own string, so testing in the
// other order would report a slot this plugin had just moved as still holding the
// incumbent. One read, then plain compares, so a slot IOSU won't answer for is reported
// as unreadable rather than as anything about its contents.
//
// The read is as wide as either module's slot and the caller keeps it, so the layout
// line can be judged on the same bytes the long line was. True when the read itself
// worked, whatever it found.
bool observe_slot(const IosuHandle &iosu, const Slot &slot, unsigned char *bytes,
                  size_t bytes_size) {
    // Four failures rather than one word for all of them. A console with no Mocha at
    // all, a process that could not open it, and a Mocha that works while this
    // particular address will not answer are different faults: the first two say
    // nothing about the address, and the last says the address is wrong or out of
    // reach.
    if (!iosu_ready()) {
        std::snprintf(slot.seen, slot.seen_size, "no IOSU");
        return false;
    }

    if (!iosu.is_open()) {
        std::snprintf(slot.seen, slot.seen_size, "no IOSU in this process");
        return false;
    }

    // "Yet" because that's what it means. These read as unavailable through an early
    // cold boot and correctly afterwards, so the address isn't the suspect.
    const size_t width = widest(slot);
    if (width > bytes_size || !iosu.read_bytes(slot.address, bytes, width)) {
        std::snprintf(slot.seen, slot.seen_size, "not readable yet at %08x",
                      static_cast<unsigned>(slot.address));
        return false;
    }

    if (slot.roseverse_length <= width &&
        std::memcmp(bytes, slot.roseverse, slot.roseverse_length) == 0) {
        std::snprintf(slot.seen, slot.seen_size, "%s", slot.roseverse_host);
        return true;
    }

    if (slot.pretendo_length <= width &&
        std::memcmp(bytes, slot.pretendo, slot.pretendo_length) == 0) {
        std::snprintf(slot.seen, slot.seen_size, "%s", slot.pretendo_host);
        return true;
    }

    for (const Incumbent *incumbent : {&slot.api, &slot.npts}) {
        if (incumbent->length <= width &&
            std::memcmp(bytes, incumbent->bytes, incumbent->length) == 0) {
            std::snprintf(slot.seen, slot.seen_size, "%s", incumbent->host);
            return true;
        }
    }

    // Some other build wrote here, or the address is not what this thinks it is.
    // Either way the honest answer is that this plugin doesn't know the host, not a
    // guess at which network it belongs to.
    std::snprintf(slot.seen, slot.seen_size, "not recognized");
    return true;
}

} // namespace

void spotpass_observe() {
    // One handle for all four, opened in the process the menu is open in.
    const IosuHandle iosu;
    for (const Slot &slot : SLOTS) {
        unsigned char bytes[WIDEST_SLOT] = {};
        const bool read = observe_slot(iosu, slot, bytes, sizeof(bytes));
        if (&slot == &LONG_SLOT) {
            layout_seen = read ? layout_shown(slot, bytes) : Layout::NotRead;
        }
    }
}

const char *spotpass_layout_name() {
    return layout_name(layout_seen);
}

const char *spotpass_long_seen() {
    return long_seen;
}

const char *spotpass_short_seen() {
    return short_seen;
}

const char *spotpass_policy_seen() {
    return policy_seen;
}

const char *spotpass_policy_host_seen() {
    return policy_host_seen;
}

void spotpass_apply(SpotPass chosen) {
    const Destination destination = destination_for(chosen);

    if (destination == Destination::LeaveAlone) {
        // Leaving all four alone is how Protarium keeps SpotPass, and it's also what
        // makes the default indistinguishable from this plugin being absent. They are
        // never written back the other way either: their module already restores them
        // at the next boot, and a plugin racing a module to own four addresses is a
        // worse deal than deferring to it. So switching back takes a reboot, which is
        // what the menu says.
        return;
    }

    if (!iosu_ready()) {
        return;
    }

    // The setting changed without a reboot, and nothing more moves until there's one.
    //
    // This is the case that makes the all-or-nothing matter. Part of what this file
    // writes is already pointing at the other network, and the slots don't all refuse
    // a second move for the same reason: a tasksheet holding Pretendo's URL no longer
    // matches the incumbent and stops itself, while the policy pair still holds exactly
    // what it's checked against and would move perfectly happily. Letting that happen
    // would leave tasksheets on one network and the policy list on the other, which is
    // the split this file exists to prevent, and it would be this plugin causing it
    // rather than preventing it.
    //
    // So nothing is written, all four say what's true of them, and the reboot the menu
    // already asks for is what applies the new setting. Their module restores its own
    // values at boot, and this starts again from a state it can prove.
    if (have_applied && applied != destination) {
        for (const Slot &slot : SLOTS) {
            report(slot, "reboot to move");
        }
        LOG("SpotPass destination changed mid-session, leaving all four until a reboot");
        return;
    }

    // One handle for all four slots and the refresh, opened in this title's own
    // process. Checked before anything is recorded as applied, so a process that cannot
    // open one leaves the next title start to try, the way a failed read does.
    const IosuHandle iosu;
    if (!iosu.is_open()) {
        return;
    }

    applied = destination;
    have_applied = true;

    // Once per boot. A read that failed or proved neither module gets another turn at
    // the next application start, since nothing was written on either.
    if (layout != Layout::Api && layout != Layout::Npts) {
        layout = recognize(iosu, destination);
        if (layout == Layout::NotRecognized) {
            report_already_moved(iosu, destination);
        }
        if (layout != Layout::Api && layout != Layout::Npts) {
            return;
        }
    }

    bool moved = false;
    for (const Slot &slot : SLOTS) {
        // Once the long slot has proved a module, each slot stands on its own guard, so
        // one that fails validation is skipped rather than taking the others down with
        // it. Part of a move is worth having: the tasksheets are the same URL in two
        // shapes and the console picks by which shape a request needs, so a slot that
        // moved serves the new network whatever the others do.
        if (concerns(slot, destination)) {
            moved = move_slot(iosu, slot, incumbent_for(slot, layout), destination) || moved;
        }
    }

    if (moved) {
        iosu.write_word(BOSS_POLICY_REFRESH_ADDRESS, BOSS_POLICY_REFRESH_VALUE);
    }
}

const char *spotpass_long_status() {
    return long_status;
}

const char *spotpass_short_status() {
    return short_status;
}

bool spotpass_moves_policy(SpotPass chosen) {
    // Or has already reported on them this boot, which isn't the same question. A
    // console that moved the pair and then had the Miiverse changed under it would
    // otherwise hide the two lines at exactly the moment they stop agreeing with the
    // setting, which is when they're most worth reading.
    //
    // Pretendo shows them that way only, because it only reports on them once the long
    // slot has proved the newer module. On the older one they stay hidden as they always
    // have, since Pretendo has nothing to move there.
    return destination_for(chosen) == Destination::Roseverse || policy_reported;
}

const char *spotpass_policy_status() {
    return policy_status;
}

const char *spotpass_policy_host_status() {
    return policy_host_status;
}
