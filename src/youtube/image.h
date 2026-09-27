#pragma once

#include <cstdint>

// Where the app's own lb_shell.rpx sits in memory. The loader relocates the whole image, so its
// code and its constants are not at the addresses the rpx was linked at, and every patch finds
// what it is after from here.

// The rpx's layout as linked. Text runs from .syscall to the end of .text and data from .rodata
// to the end of .data, and each region moves as one block.
#define IMAGE_TEXT_LINK   0x02000000u
#define IMAGE_TEXT_END    0x04026da8u
#define IMAGE_DATA_LINK   0x10000000u
#define IMAGE_RODATA_END  0x103790e0u
#define IMAGE_DATA_END    0x1038a674u

// Finds the image and records how, for the trace. Call once from the application start hook,
// before any patch. Answers whether the regions moved from where they were linked were found;
// false leaves the link addresses in place as the last guess.
bool image_locate();

// How the image was found, or that it was not.
const char *image_how();

// Where a link address in the rpx is now. Only meaningful for addresses inside the regions above.
uint32_t image_text(uint32_t link);
uint32_t image_data(uint32_t link);
