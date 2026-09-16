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

#include "notify.h"
#include "logger.h"

#include <notifications/notifications.h>

#include <cstdio>

namespace {

// Fifteen seconds before the toast fades out.
//
// A double rather than a float, because the option goes through varargs and the header
// says outright that DURATION_BEFORE_FADE_OUT expects a double. 15.0f would work too,
// since default argument promotion widens it on the way, but then the code leans on the
// promotion instead of stating the contract.
constexpr double FADE_OUT_SECONDS = 15.0;

constexpr const char *account_text(Account account) {
    return account == Account::Pretendo ? "Pretendo" : "Protarium";
}

// The Miiverse as the toast spells it, which isn't quite miiverse_name().
//
// The accented spelling belongs to the notification and to nothing else here: the menu
// and the Debug page say "Roseverse", same as every other line there, and changing those
// wasn't asked for.
//
// The bytes are written as escapes rather than as a literal accented character. The
// spelling is UTF-8, "Ros\xc3\xa9verse". An escape can't be mangled by an editor or by
// a compiler's idea of the source encoding. A pasted character can.
//
// Safe as written because the escape is followed by 'v', which isn't a hex digit.
// Anything added later that puts a hex digit after one of these has to break the literal
// in two, or the escape swallows it.
//
// Every value named rather than defaulted, so a fourth Miiverse added later warns here
// instead of quietly arriving with no toast.
constexpr const char *miiverse_text(Miiverse miiverse) {
    switch (miiverse) {
        case Miiverse::Protaverse: return "Protaverse";
        case Miiverse::Juxt: return "Juxt";
        case Miiverse::Roseverse: return "Ros\xc3\xa9verse";
    }
    return nullptr;
}

// The toast's text lives as long as the plugin rather than on the stack. The module
// holds the notification until the overlay exists, which is after notify_boot has
// returned.
char toast_text[48];

// Whether this file initialized the library, so shutdown releases exactly what was taken
// and nothing else.
//
// Not a once-per-boot flag, and there's no once-per-boot flag here: the hook already runs
// once, as notify.h sets out. This one answers a different question the hook can't. A
// console with no NotificationModule reaches notify_boot, fails at the first call, and
// must not then have DeInitLibrary called on a library that was never initialized.
bool holding = false;

char toast_status[40] = "not run";

} // namespace

void notify_boot(Account account, Miiverse miiverse) {
    const char *spelled = miiverse_text(miiverse);
    if (spelled == nullptr) {
        // Config::Init stores nothing but the three it names, so reaching here means a
        // Miiverse was added to choice.h without a spelling above.
        std::snprintf(toast_status, sizeof(toast_status), "no text for Miiverse %u",
                      static_cast<unsigned>(miiverse));
        return;
    }
    std::snprintf(toast_text, sizeof(toast_text), "%s + %s Enabled", account_text(account),
                  spelled);

    const NotificationModuleStatus init = NotificationModule_InitLibrary();
    if (init != NOTIFICATION_MODULE_RESULT_SUCCESS) {
        std::snprintf(toast_status, sizeof(toast_status), "init failed (%d)",
                      static_cast<int>(init));
        LOG("no boot toast, NotificationModule init returned %d", static_cast<int>(init));
        return;
    }
    holding = true;

    // Both defaults before the notification, in this order. KEEP_UNTIL_SHOWN is the one
    // that matters: the overlay doesn't exist when a plugin initializes, so without it
    // this notification is posted into nothing and the toast never appears.
    const NotificationModuleStatus keep = NotificationModule_SetDefaultValue(
        NOTIFICATION_MODULE_NOTIFICATION_TYPE_INFO,
        NOTIFICATION_MODULE_DEFAULT_OPTION_KEEP_UNTIL_SHOWN, true);

    const NotificationModuleStatus fade = NotificationModule_SetDefaultValue(
        NOTIFICATION_MODULE_NOTIFICATION_TYPE_INFO,
        NOTIFICATION_MODULE_DEFAULT_OPTION_DURATION_BEFORE_FADE_OUT, FADE_OUT_SECONDS);

    if (keep != NOTIFICATION_MODULE_RESULT_SUCCESS ||
        fade != NOTIFICATION_MODULE_RESULT_SUCCESS) {
        // Stop here rather than post with whatever the defaults happened to be. A toast
        // that vanishes before it's read is worse than none, because it looks like the
        // plugin did nothing.
        std::snprintf(toast_status, sizeof(toast_status), "defaults refused (%d, %d)",
                      static_cast<int>(keep), static_cast<int>(fade));
        return;
    }

    const NotificationModuleStatus shown = NotificationModule_AddInfoNotification(toast_text);
    if (shown != NOTIFICATION_MODULE_RESULT_SUCCESS) {
        std::snprintf(toast_status, sizeof(toast_status), "refused (%d)",
                      static_cast<int>(shown));
        return;
    }

    std::snprintf(toast_status, sizeof(toast_status), "queued");
    LOG("boot toast queued: %s", toast_text);
}

void notify_shutdown() {
    if (!holding) {
        return;
    }
    holding = false;
    NotificationModule_DeInitLibrary();
}

const char *notify_status() {
    return toast_status;
}
