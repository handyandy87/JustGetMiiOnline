#pragma once

#include <cstdint>

// Finding, reading and writing bytes in the running app. Its code and its constants are mapped
// read only and sit behind the caches, so a write goes through the kernel and the caches are
// told after. image.h says where the app is.

// The first address in the range holding these bytes, or 0. Only reads pages that are mapped,
// and will not report a match lying across the join between a mapped page and an unmapped one.
// Step is 4 for instructions, which are aligned, and 1 for anything else.
uint32_t patch_find(uint32_t start, uint32_t end, const void *needle, uint32_t length,
                    uint32_t step);

// Copies bytes out through the kernel, so reading never faults whatever the page allows. Answers
// false when the address has no physical page behind it. Length is capped at PATCH_MAX.
bool patch_read(uint32_t address, void *out, uint32_t length);

// Writes over whatever is there, then reads it back. Answers whether what is now at the address
// is what was asked for. Length is capped at PATCH_MAX.
#define PATCH_MAX 64u
bool patch_write(uint32_t address, const void *bytes, uint32_t length);
