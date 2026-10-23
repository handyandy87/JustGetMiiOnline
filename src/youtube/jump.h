#pragma once

// The video a Miiverse post's jump starts the app at. The system hands the jump over as the
// 2,096 byte Miiverse data argument. The video's id is in the post's app data when it has some,
// and otherwise in the post's id.

#include <cstdint>

#define JUMP_ID_BYTES 11

// Whether the 2,096 byte Miiverse data block is a jump to a video, and if so the id in `id`,
// which takes JUMP_ID_BYTES + 1. `id` is left alone otherwise. Reads in place, never past `size`,
// and keeps nothing but the id.
bool jump_parse(const void *block, uint32_t size, char *id);

// Reads this run's Miiverse data argument. Call once, from the application start hook, in the title
// the jump is for. A run started any other way has no id, whatever an earlier run had.
void jump_read();

// The id jump_read took, or null when the run didn't start from a jump to a video.
const char *jump_video_id();
