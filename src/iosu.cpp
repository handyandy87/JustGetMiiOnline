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

#include "iosu.h"
#include "logger.h"

#include <coreinit/ios.h>

#include <cstring>

namespace {

// Mocha's resource manager, and the two requests this file makes of it. The numbers
// are MochaPayload's, from the ioctl switch in its ios_mcp ipc.c, and libmocha sends
// the same ones.
constexpr const char *MOCHA_DEVICE = "/dev/iosuhax";
constexpr uint32_t IOCTL_KERN_READ32 = 0x06;
constexpr uint32_t IOCTL_KERN_WRITE32 = 0x07;

bool ready = false;

} // namespace

bool iosu_init() {
    // Only whether Mocha is there to be asked. This handle closes again at once.
    const IosuHandle probe;
    ready = probe.is_open();
    if (!ready) {
        LOG("Mocha unavailable, nothing here can reach IOSU");
    }
    return ready;
}

void iosu_shutdown() {
    // No handle outlives the operation that opened it, so there's nothing to close.
    ready = false;
}

bool iosu_ready() {
    return ready;
}

// Opened here, in whichever process is doing the work.
//
// This used to be libmocha's handle, and libmocha keeps exactly one: opened when the
// plugin initializes and used from then on. An IOS handle belongs to the process that
// opened it, and this plugin initializes once per boot, in the first Wii U Menu. So
// every later process, each title and any Wii U Menu started again after one, sent its
// requests down a number that wasn't its own. Hardware showed it: inside Wii U Chat
// the account address and both SpotPass addresses read as nothing this plugin
// recognizes, while Protarium's services, Wii U Chat among them, kept working. The
// addresses were fine. The reads just weren't reads of them.
//
// Nothing was ever written that way, because every write in this plugin waits for a
// read to match first.
IosuHandle::IosuHandle() : handle(IOS_Open(MOCHA_DEVICE, static_cast<IOSOpenMode>(0))) {}

IosuHandle::~IosuHandle() {
    if (handle >= 0) {
        IOS_Close(handle);
    }
}

bool IosuHandle::is_open() const {
    return handle >= 0;
}

// Both buffers aligned the way libmocha aligns its own, since IPC hands them to IOSU
// as they are.
bool IosuHandle::read_word(uint32_t address, uint32_t &out) const {
    alignas(0x40) uint32_t request[0x40 / 4] = {address};
    alignas(0x40) uint32_t reply[0x40 / 4] = {};
    if (IOS_Ioctl(handle, IOCTL_KERN_READ32, request, sizeof(uint32_t), reply,
                  sizeof(uint32_t)) < 0) {
        return false;
    }
    out = reply[0];
    return true;
}

bool IosuHandle::write_word(uint32_t address, uint32_t value) const {
    if (!is_open()) {
        return false;
    }
    alignas(0x40) uint32_t request[0x40 / 4] = {address, value};
    return IOS_Ioctl(handle, IOCTL_KERN_WRITE32, request, 2 * sizeof(uint32_t), nullptr, 0) >= 0;
}

bool IosuHandle::read_bytes(uint32_t address, void *out, size_t length) const {
    if (!is_open()) {
        return false;
    }

    auto *bytes = static_cast<char *>(out);
    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t word = 0;
        if (!read_word(address + offset, word)) {
            return false;
        }
        const size_t span = (length - offset < 4) ? length - offset : 4;
        std::memcpy(bytes + offset, &word, span);
    }
    return true;
}

bool IosuHandle::write_bytes(uint32_t address, const void *data, size_t length) const {
    if (!is_open()) {
        LOG("no Mocha handle in this process to write at %08x", address);
        return false;
    }

    const auto *bytes = static_cast<const char *>(data);
    const size_t whole = length - (length % 4);

    for (size_t offset = 0; offset < whole; offset += 4) {
        uint32_t word = 0;
        std::memcpy(&word, bytes + offset, sizeof(word));
        if (!write_word(address + offset, word)) {
            LOG("IOSU write failed at %08x", address + offset);
            return false;
        }
    }

    const size_t remainder = length % 4;
    if (remainder > 0) {
        uint32_t tail = 0;
        if (!read_word(address + whole, tail)) {
            LOG("IOSU read failed at %08x, leaving the write half done", address + whole);
            return false;
        }
        std::memcpy(&tail, bytes + whole, remainder);
        if (!write_word(address + whole, tail)) {
            LOG("IOSU write failed at %08x", address + whole);
            return false;
        }
    }

    return true;
}

bool IosuHandle::matches(uint32_t address, const void *image, size_t length) const {
    if (!is_open()) {
        return false;
    }

    const auto *want = static_cast<const char *>(image);

    for (size_t offset = 0; offset < length; offset += 4) {
        uint32_t current = 0;
        if (!read_word(address + offset, current)) {
            return false;
        }

        // Only the span that is actually being compared is taken from the image.
        // The rest of the word keeps what was read, so the bytes past the end of a
        // short run never decide the answer.
        uint32_t expected = current;
        const size_t span = (length - offset < 4) ? length - offset : 4;
        std::memcpy(&expected, want + offset, span);

        if (current != expected) {
            return false;
        }
    }
    return true;
}
