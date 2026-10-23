// The jump's data is 2,096 bytes, its numbers big-endian. The flags word is at 0, the app data at
// 521 with room for 1,024 bytes, its length at 1,548, and the post's id at 1,552 in 32 bytes. The
// rest is the community, a service token at 8 and two host names, and none of it is read here.

#include "jump.h"

#include <cstddef>
#include <cstring>
#include <sysapp/args.h>

#define PAYLOAD_BYTES      2096u
#define FLAGS_AT           0u
#define HAS_APP_DATA       0x4u
#define APP_DATA_AT        521u
#define APP_DATA_LENGTH_AT 1548u
#define POST_ID_AT         1552u

static char sId[JUMP_ID_BYTES + 1];
static bool sHasId = false;

static uint32_t word(const uint8_t *at) {
    return (uint32_t) at[0] << 24 | (uint32_t) at[1] << 16 | (uint32_t) at[2] << 8 | at[3];
}

static bool id_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

static bool id_at(const uint8_t *at) {
    for (uint32_t i = 0; i < JUMP_ID_BYTES; i++) {
        if (!id_char(at[i])) {
            return false;
        }
    }
    return true;
}

bool jump_parse(const void *block, uint32_t size, char *id) {
    if (!id || !block || size != PAYLOAD_BYTES) {
        return false;
    }
    const uint8_t *data = (const uint8_t *) block;
    const uint8_t *found = nullptr;
    if ((word(data + FLAGS_AT) & HAS_APP_DATA) && word(data + APP_DATA_LENGTH_AT) == JUMP_ID_BYTES &&
        id_at(data + APP_DATA_AT)) {
        found = data + APP_DATA_AT;
    } else if (id_at(data + POST_ID_AT) && !data[POST_ID_AT + JUMP_ID_BYTES]) {
        found = data + POST_ID_AT;
    }
    if (!found) {
        return false;
    }
    memcpy(id, found, JUMP_ID_BYTES);
    id[JUMP_ID_BYTES] = 0;
    return true;
}

// The system owns the block's memory and wut has no call to give it back, so it is only read.
void jump_read() {
    sHasId = false;
    memset(sId, 0, sizeof(sId));
    SYSArgDataBlock blocks[2];
    memset(blocks, 0, sizeof(blocks));
    blocks[0].id = SYS_ARG_ID_MIV_DATA;
    blocks[0].type = SYS_ARG_TYPE_DATA;
    blocks[1].id = SYS_ARG_ID_END;
    SYSCallerInfo caller;
    memset(&caller, 0, sizeof(caller));
    if (SYSGetArguments(blocks, &caller) == 0) {
        sHasId = jump_parse(blocks[0].data.ptr, blocks[0].data.size, sId);
    }
}

const char *jump_video_id() { return sHasId ? sId : nullptr; }
