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

#pragma once

#include <cstddef>
#include <cstdint>

// SHA-256, for checking a downloaded update against the checksum its release publishes.
// Nothing already in this build offers one: wut doesn't, and the mbedTLS inside
// CURLWrapperModule exports only curl. So this is a small one written from FIPS 180-4,
// fed in pieces as the download arrives, so the file never has to sit in memory whole.
struct Sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
};

void sha256_init(Sha256 *ctx);
void sha256_update(Sha256 *ctx, const void *data, size_t size);
void sha256_final(Sha256 *ctx, uint8_t digest[32]);
