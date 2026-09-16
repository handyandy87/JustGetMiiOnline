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

#include "secret.h"
#include "logger.h"

#include <wups/storage.h>

#include <zlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *KEY_SECRET = "roseverse_secret";

// The name the constant lives under in their module. Stable as long as they don't
// rename it, and it has held across every release so far.
constexpr const char *SECRET_SYMBOL = "_ZZL16obfuscate_stringPcjE6secret";

// The symbol I look for to tell that the file really is Project Rose's module.
// Checked before the constant, only so two failures can be told apart. Somebody
// who points this at Protarium's module should be told they picked the wrong
// file, not that Project Rose renamed something.
constexpr const char *ROSE_MARKER = "_Z16init_olive_tokenv";

constexpr uint32_t SHF_RPL_ZLIB = 0x08000000;
constexpr uint32_t SHT_NOBITS = 8;

// A module that doesn't fit these isn't one I can read, and saying so beats
// allocating whatever a corrupt header asks for.
constexpr uint32_t MAX_SECTIONS = 64;
constexpr uint32_t MAX_INFLATED = 1u << 20;

ImportState g_state = ImportState::NotImported;
const char *g_failure = nullptr;
uint8_t g_secret[SECRET_LEN];
bool g_have_secret = false;

struct Section {
    uint32_t nameoff, type, flags, addr, off, size;
};

// Assembled a byte at a time rather than read as a struct, so nothing here depends on
// how the compiler lays a struct out.
uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t be16(const uint8_t *p) {
    return uint16_t((uint32_t(p[0]) << 8) | p[1]);
}

bool read_at(std::FILE *f, long off, void *out, size_t len) {
    return std::fseek(f, off, SEEK_SET) == 0 && std::fread(out, 1, len, f) == len;
}

// One section's bytes, ready to read.
//
// A section that can't be read back gets refused here rather than handed over the way it
// sits in the file. An importer that quietly handed that back would find no symbol and
// report the wrong reason for it.
uint8_t *inflate_section(std::FILE *f, const Section &sec, uint32_t *out_len) {
    *out_len = 0;
    if (sec.type == SHT_NOBITS || sec.size == 0) return nullptr;

    uint8_t *raw = static_cast<uint8_t *>(std::malloc(sec.size));
    if (!raw) return nullptr;
    if (!read_at(f, static_cast<long>(sec.off), raw, sec.size)) {
        std::free(raw);
        return nullptr;
    }

    if (!(sec.flags & SHF_RPL_ZLIB)) {
        *out_len = sec.size;
        return raw;
    }
    if (sec.size < 4) {
        std::free(raw);
        return nullptr;
    }

    const uint32_t want = be32(raw);
    if (want == 0 || want > MAX_INFLATED) {
        std::free(raw);
        return nullptr;
    }
    uint8_t *out = static_cast<uint8_t *>(std::malloc(want));
    if (!out) {
        std::free(raw);
        return nullptr;
    }

    uLongf got = want;
    const int rc = uncompress(out, &got, raw + 4, sec.size - 4);
    std::free(raw);
    if (rc != Z_OK) {
        std::free(out);
        return nullptr;
    }
    *out_len = static_cast<uint32_t>(got);
    return out;
}

bool name_equals(const uint8_t *strings, uint32_t strings_len, uint32_t off, const char *want) {
    if (off >= strings_len) return false;
    const char *have = reinterpret_cast<const char *>(strings + off);
    const size_t room = strings_len - off;
    const size_t len = std::strlen(want);
    return len < room && std::strncmp(have, want, room) == 0 && have[len] == '\0';
}

// The whole read, with every exit carrying the sentence the menu should show.
// Failure reasons differ because the actions they call for differ: a wrong file,
// a stripped build and a changed key length each need something else done.
bool extract(const char *path, uint8_t out[SECRET_LEN], const char **why) {
    *why = "could not be read";

    std::FILE *f = std::fopen(path, "rb");
    if (!f) {
        // Deliberately doesn't say the file is missing. An open fails the same
        // way when the file is absent, when the path is wrong, and when no
        // filesystem device is registered for this plugin to ask, and the first
        // hardware attempt hit the third of those while being told it was the
        // first. Name the path and let the reader check it.
        *why = "could not open wiiu/roseverse.wms on the SD card";
        return false;
    }

    uint8_t hdr[0x34];
    if (!read_at(f, 0, hdr, sizeof(hdr)) || std::memcmp(hdr, "\x7f" "ELF", 4) != 0) {
        *why = "that file is not a module";
        std::fclose(f);
        return false;
    }

    const uint32_t shoff = be32(hdr + 0x20);
    const uint16_t shentsize = be16(hdr + 0x2E);
    const uint16_t shnum = be16(hdr + 0x30);
    const uint16_t shstrndx = be16(hdr + 0x32);
    if (shnum == 0 || shnum > MAX_SECTIONS || shentsize < 40 || shstrndx >= shnum) {
        *why = "that module's section table is not readable";
        std::fclose(f);
        return false;
    }

    Section *secs = static_cast<Section *>(std::calloc(shnum, sizeof(Section)));
    if (!secs) {
        *why = "not enough memory to read it";
        std::fclose(f);
        return false;
    }
    bool ok = true;
    for (uint16_t i = 0; i < shnum && ok; ++i) {
        uint8_t e[40];
        ok = read_at(f, static_cast<long>(shoff + i * shentsize), e, sizeof(e));
        secs[i] = {be32(e), be32(e + 4), be32(e + 8), be32(e + 12), be32(e + 16), be32(e + 20)};
    }
    if (!ok) {
        *why = "that module's section table is not readable";
        std::free(secs);
        std::fclose(f);
        return false;
    }

    // The section names go through the same read as everything else, or every section
    // comes back called garbage.
    uint32_t shstr_len = 0;
    uint8_t *shstr = inflate_section(f, secs[shstrndx], &shstr_len);
    if (!shstr) {
        *why = "that module's section names did not decompress";
        std::free(secs);
        std::fclose(f);
        return false;
    }

    int symtab = -1, strtab = -1;
    for (uint16_t i = 0; i < shnum; ++i) {
        if (name_equals(shstr, shstr_len, secs[i].nameoff, ".symtab")) symtab = i;
        else if (name_equals(shstr, shstr_len, secs[i].nameoff, ".strtab")) strtab = i;
    }
    std::free(shstr);

    if (symtab < 0 || strtab < 0) {
        // A stripped build is the expected shape of this, not a corrupt one. Say
        // which, so nobody goes looking for a bad download.
        *why = "that module was built stripped, so the constant cannot be named";
        std::free(secs);
        std::fclose(f);
        return false;
    }

    uint32_t syms_len = 0, names_len = 0;
    uint8_t *syms = inflate_section(f, secs[symtab], &syms_len);
    uint8_t *names = syms ? inflate_section(f, secs[strtab], &names_len) : nullptr;
    if (!syms || !names) {
        *why = "that module's symbol table did not decompress";
        std::free(syms);
        std::free(names);
        std::free(secs);
        std::fclose(f);
        return false;
    }

    bool is_rose = false;
    bool found = false;
    uint32_t addr = 0, size = 0;
    for (uint32_t o = 0; o + 16 <= syms_len; o += 16) {
        const uint32_t nameoff = be32(syms + o);
        if (!is_rose && name_equals(names, names_len, nameoff, ROSE_MARKER)) {
            is_rose = true;
        } else if (!found && name_equals(names, names_len, nameoff, SECRET_SYMBOL)) {
            addr = be32(syms + o + 4);
            size = be32(syms + o + 8);
            found = true;
        }
        if (is_rose && found) break;
    }
    std::free(syms);
    std::free(names);

    if (!is_rose) {
        *why = "that module has no token builder, so it is not Project Rose's";
        std::free(secs);
        std::fclose(f);
        return false;
    }
    if (!found) {
        *why = "their module no longer carries the constant under that name";
        std::free(secs);
        std::fclose(f);
        return false;
    }
    if (size != SECRET_STORED) {
        // A constant of any other length isn't one this build can use, so it gets
        // refused rather than stored and quietly turned into a token that fails.
        LOG("roseverse import: constant is %u bytes, expected %u", size,
            static_cast<unsigned>(SECRET_STORED));
        *why = "their key length changed, so this build cannot use it";
        std::free(secs);
        std::fclose(f);
        return false;
    }

    // The section holding an address is the one with the highest start at or
    // below it, because loaded sections don't overlap. Taking the first section
    // that merely starts below the address would pick .text for everything, and
    // the only thing between that and a wrong answer would be the bounds check
    // further down. A section's recorded size isn't the size it reads back at, so it
    // can't bound the search and the ordering has to.
    int holder = -1;
    for (uint16_t i = 0; i < shnum; ++i) {
        if (!secs[i].addr || secs[i].type == SHT_NOBITS || !secs[i].size) continue;
        if (addr < secs[i].addr) continue;
        if (holder < 0 || secs[i].addr > secs[holder].addr) holder = i;
    }
    if (holder < 0) {
        *why = "the constant is not in any loaded section";
        std::free(secs);
        std::fclose(f);
        return false;
    }

    uint32_t data_len = 0;
    uint8_t *data = inflate_section(f, secs[holder], &data_len);
    const uint32_t off = addr - secs[holder].addr;
    std::free(secs);
    std::fclose(f);
    if (!data) {
        *why = "the section holding the constant did not decompress";
        return false;
    }
    if (off + SECRET_STORED > data_len) {
        std::free(data);
        *why = "the constant runs past the end of its section";
        return false;
    }

    // Both checks catch a read that landed in the wrong place, which is the one
    // failure that would otherwise store sixteen plausible bytes and be found out
    // only by a sign-in that doesn't work.
    if (data[off + SECRET_LEN] != 0) {
        std::free(data);
        *why = "the constant is not terminated, so the read landed wrong";
        return false;
    }
    for (uint32_t i = 0; i < SECRET_LEN; ++i) {
        if (data[off + i] < 32 || data[off + i] > 126) {
            std::free(data);
            *why = "the constant holds a non-printable byte, so the read landed wrong";
            return false;
        }
    }

    std::memcpy(out, data + off, SECRET_LEN);
    std::free(data);
    return true;
}

} // namespace

void secret_init() {
    uint8_t buf[SECRET_LEN];
    uint32_t got = 0;
    const WUPSStorageError err =
        WUPSStorageAPI_GetBinary(nullptr, KEY_SECRET, buf, sizeof(buf), &got);

    if (err != WUPS_STORAGE_ERROR_SUCCESS || got != SECRET_LEN) {
        // Not found is the ordinary case on a console that has never run the
        // action, and any other error gets the same answer for the same reason
        // the two settings do: an unreadable value degrades to doing nothing.
        g_have_secret = false;
        g_state = ImportState::NotImported;
        return;
    }
    std::memcpy(g_secret, buf, SECRET_LEN);
    g_have_secret = true;
    g_state = ImportState::Imported;
}

ImportState secret_import() {
    uint8_t found[SECRET_LEN];
    const char *why = nullptr;

    if (!extract(SECRET_SOURCE_PATH, found, &why)) {
        // The stored value is deliberately left alone. A run pointed at the wrong
        // file should cost the user an error message, not the constant they
        // imported correctly last month.
        g_failure = why;
        g_state = ImportState::Failed;
        LOG("roseverse import failed: %s", why);
        return g_state;
    }

    const WUPSStorageError err =
        WUPSStorageAPI_StoreBinary(nullptr, KEY_SECRET, found, SECRET_LEN);
    if (err != WUPS_STORAGE_ERROR_SUCCESS) {
        g_failure = "read it, but could not save it";
        g_state = ImportState::Failed;
        LOG("roseverse import: storage failed: %s", WUPSStorageAPI_GetStatusStr(err));
        return g_state;
    }

    std::memcpy(g_secret, found, SECRET_LEN);
    g_have_secret = true;
    g_failure = nullptr;
    g_state = ImportState::Imported;
    // I never log the value itself. It's the one thing this whole mechanism
    // exists to keep out of anything that gets shared, and a log is shared.
    LOG("roseverse import: %u bytes stored", static_cast<unsigned>(SECRET_LEN));
    return g_state;
}

ImportState secret_state() {
    return g_state;
}

const char *secret_failure() {
    return g_state == ImportState::Failed ? g_failure : nullptr;
}

const uint8_t *secret_bytes() {
    return g_have_secret ? g_secret : nullptr;
}
