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

#include "plaza.h"
#include "logger.h"

#include <coreinit/debug.h>
#include <nn/act/client_cpp.h>
#include <nn/result.h>

#include <cstdio>
#include <cstring>

// nn_boss has link stubs in wut but no header.
//
// libwut.a carries the whole nn::boss surface as per-symbol
// .fimport_nn_boss.<mangled> sections inside nn_boss.o, the same way the nn::act
// functions this file also uses are carried in nn_act.o. What it doesn't ship is a
// header, so the five entry points are declared here in wut's own idiom: a plain
// prototype with the mangled name in an asm label. Nothing is reimplemented. These
// resolve to the console's own nn_boss.rpl at load time, and -lwut already covers
// them, so the Makefile is unchanged.
//
// DELIBERATELY OUTSIDE THE ANONYMOUS NAMESPACE BELOW. Inside it these get internal
// linkage, and an internal-linkage function with an asm label is not a reference the
// linker goes looking for: the compiler warns that each was "declared static but
// never defined" and only happens to emit the external reference anyway. The import
// section came out correct either way, which is exactly what makes it a bad thing to
// lean on. External linkage is what wut's own nn::act declarations use, and it's the
// only spelling that says what is meant.
//
// Task's member functions are declared as free functions taking the object. On
// PowerPC the implicit argument is just the first one, so the call is identical, and
// it saves declaring a layout for a class whose real shape isn't published anywhere
// I could check it against.
namespace boss {

nn::Result Initialize() asm("Initialize__Q2_2nn4bossFv");
nn::Result Finalize() asm("Finalize__Q2_2nn4bossFv");

void TaskConstruct(void *task) asm("__ct__Q3_2nn4boss4TaskFv");
nn::Result TaskInitialize(void *task, const char *task_id, uint32_t account)
    asm("Initialize__Q3_2nn4boss4TaskFPCcUi");
nn::Result TaskUnregister(void *task) asm("Unregister__Q3_2nn4boss4TaskFv");

} // namespace boss

namespace {

// How much stack to give an object whose size I can't know from here.
//
// I have one measurement of the size and nothing else: 32 bytes zeroed before the
// constructor is called. This doubles that rather than matching it. Too generous
// costs 32 bytes of a config-menu stack frame. Too small lets the constructor write
// through the frame, and that's the kind of fault that turns up somewhere else
// entirely and never gets traced back here.
constexpr size_t TASK_STORAGE = 64;

// The task that feeds the plaza, and the one string in this file I had to recover
// rather than reason out. It is the id handed to Task::Initialize below.
constexpr const char TASK_ID[] = "oltopic";

// The Wii U Menu. Same number the Debug page prints beside the discovery host and
// the pre-main trigger, so you can check this against a line on screen.
constexpr uint32_t WII_U_MENU_UPID = 2;

char status[64] = "not reset this session";

} // namespace

bool plaza_available() {
    return OSGetUPID() == WII_U_MENU_UPID;
}

const char *plaza_status() {
    return status;
}

size_t plaza_reset() {
    // Checked again here instead of trusting the menu. The menu hides the item
    // where this is false, but a callback that is only correct because of what the
    // caller did is one refactor away from being wrong.
    if (!plaza_available()) {
        std::snprintf(status, sizeof(status), "needs the Wii U Menu");
        return 0;
    }

    nn::act::Initialize();
    boss::Initialize();

    // Slots are one-based and there are never many, but the loop counter is wider
    // than the slot, so a console claiming 255 accounts ends the loop instead of
    // wrapping it.
    const unsigned count = nn::act::GetNumOfAccounts();
    size_t done = 0;
    size_t local = 0;

    for (unsigned i = 1; i <= count; ++i) {
        const auto slot = static_cast<nn::act::SlotNo>(i);

        if (!nn::act::IsSlotOccupied(slot)) {
            continue;
        }

        // Local accounts get skipped, not attempted. The task is keyed on a
        // persistent id belonging to a network account, so there's nothing
        // registered under a local one to remove. Counting them separately is what
        // tells "no accounts qualified" apart from "no accounts exist".
        if (!nn::act::IsNetworkAccountEx(slot)) {
            ++local;
            continue;
        }

        const uint32_t persistent_id = nn::act::GetPersistentIdEx(slot);

        // Zeroed before construction, and worth keeping: this file can't see the
        // definition, so the constructor might leave a field untouched.
        alignas(8) unsigned char task[TASK_STORAGE] = {};
        boss::TaskConstruct(task);
        boss::TaskInitialize(task, TASK_ID, persistent_id);
        boss::TaskUnregister(task);

        ++done;
    }

    boss::Finalize();
    nn::act::Finalize();

    // Three different answers, because they mean three different things. A count is
    // the success case. No network accounts means the console has nothing registered
    // and the action was never going to do anything. No accounts at all means a
    // console that isn't signed in.
    if (done > 0) {
        std::snprintf(status, sizeof(status), "reset for %u account(s)",
                      static_cast<unsigned>(done));
    } else if (local > 0) {
        std::snprintf(status, sizeof(status), "no network accounts, nothing to reset");
    } else {
        std::snprintf(status, sizeof(status), "no accounts found");
    }

    LOG("WaraWara Plaza: %s", status);
    return done;
}
