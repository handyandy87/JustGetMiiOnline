#include "image.h"

#include <coreinit/dynload.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "patch.h"
#include "trace.h"
#include "utils/logger.h"

// The one function the rpx exports, and where it was linked. Where it is now says how far the
// text region moved.
#define EXPORT_NAME "__preinit_user"
#define EXPORT_LINK 0x02221580u

// The first word of .data points at the start url, and the code at 0x02000aac loads that word
// with lis r31 and then lwz r31,d(r31). The loader rewrote both halves when it moved the data,
// so reading them back says how far the data region moved.
#define LOAD_HI_LINK 0x02000aacu
#define LOAD_LO_LINK 0x02000ab4u
#define LOAD_HI_OP   0x3fe00000u
#define LOAD_LO_OP   0x83ff0000u
#define POINTER_LINK 0x103790e0u
#define POINTEE_LINK 0x10000188u

// Added to a link address to get where it is now. Unsigned so a region that moved down wraps
// the right way.
static uint32_t sText = 0;
static uint32_t sData = 0;
static const char *sHow = "not looked for";

uint32_t image_text(uint32_t link) {
    return link + sText;
}

uint32_t image_data(uint32_t link) {
    return link + sData;
}

const char *image_how() {
    return sHow;
}

static bool is_ours(const char *name) {
    return name && strstr(name, "lb_shell") != nullptr;
}

// The loader's own record of what it placed where. Retail CafeOS answers this with nothing
// below a raised security level, so an empty answer only means asking another way.
static bool from_loader() {
    int32_t count = OSDynLoad_GetNumberOfRPLs();
    if (count <= 0) {
        trace_line("image: the loader lists no modules");
        return false;
    }
    auto *all = (OSDynLoad_NotifyData *) calloc(count, sizeof(OSDynLoad_NotifyData));
    if (!all) {
        return false;
    }
    bool found = false;
    if (!OSDynLoad_GetRPLInfo(0, count, all)) {
        trace_line("image: the loader counts %d modules but will not describe them", (int) count);
    } else {
        for (int32_t i = 0; i < count && !found; i++) {
            if (!is_ours(all[i].name)) {
                continue;
            }
            trace_line("image: the loader has %s, text 0x%08x size 0x%x, data 0x%08x size 0x%x",
                       all[i].name, all[i].textAddr, all[i].textSize, all[i].dataAddr,
                       all[i].dataSize);
            sText = all[i].textAddr - IMAGE_TEXT_LINK;
            sData = all[i].dataAddr - IMAGE_DATA_LINK;
            found = true;
        }
        if (!found) {
            trace_line("image: the loader lists %d modules, none of them this one", (int) count);
        }
    }
    free(all);
    return found;
}

// The export gives the text, and the load the code makes of the start url's pointer gives the
// data. Both are read through the kernel, which works whatever OSIsAddressValid says of a page.
static bool from_export() {
    char mainName[64] = {0};
    int32_t size = sizeof(mainName);
    // -1 asks for the main module, whatever the loader calls it
    auto self = (OSDynLoad_Module) (intptr_t) -1;
    if (OSDynLoad_GetModuleName(self, mainName, &size) != OS_DYNLOAD_OK) {
        mainName[0] = '\0';
    }
    const char *names[] = {mainName, "lb_shell", "lb_shell.rpx"};
    OSDynLoad_Module module = nullptr;
    for (const char *name : names) {
        if (name[0] && OSDynLoad_IsModuleLoaded(name, &module) == OS_DYNLOAD_OK && module) {
            break;
        }
        module = nullptr;
    }
    if (!module) {
        trace_line("image: no handle on the rpx, the main module is called '%s'", mainName);
        return false;
    }
    void *at = nullptr;
    if (OSDynLoad_FindExport(module, OS_DYNLOAD_EXPORT_FUNC, EXPORT_NAME, &at) != OS_DYNLOAD_OK ||
        !at) {
        trace_line("image: %s is not exported", EXPORT_NAME);
        return false;
    }
    sText = (uint32_t) at - EXPORT_LINK;
    trace_line("image: %s is at 0x%08x", EXPORT_NAME, (uint32_t) at);

    // Text alone is still worth having, so a data load that does not read right leaves the data
    // at its link address and says so.
    uint32_t hi = 0, lo = 0;
    if (!patch_read(image_text(LOAD_HI_LINK), &hi, sizeof(hi)) ||
        !patch_read(image_text(LOAD_LO_LINK), &lo, sizeof(lo))) {
        trace_line("image: the data load at 0x%08x does not read", image_text(LOAD_HI_LINK));
        return true;
    }
    if ((hi & 0xffff0000u) != LOAD_HI_OP || (lo & 0xffff0000u) != LOAD_LO_OP) {
        trace_line("image: the data load reads %08x %08x rather than lis and lwz", hi, lo);
        return true;
    }
    uint32_t pointer = (hi << 16) + (uint32_t) (int32_t) (int16_t) (lo & 0xffffu);
    sData = pointer - POINTER_LINK;
    return true;
}

bool image_locate() {
    sText = 0;
    sData = 0;
    if (from_loader()) {
        sHow = "the loader";
    } else if (from_export()) {
        sHow = "the export";
    } else {
        sText = 0;
        sData = 0;
        sHow = "nothing, so the link addresses";
        trace_line("image: not found, trying the link addresses");
        return false;
    }

    // The start url's pointer points at the start url wherever the data went, so this says
    // whether the data region is really where it is now thought to be.
    uint32_t pointer = 0;
    char head[5] = {0};
    bool data = patch_read(image_data(POINTER_LINK), &pointer, sizeof(pointer)) &&
                pointer == image_data(POINTEE_LINK) &&
                patch_read(image_data(POINTEE_LINK), head, 4) && memcmp(head, "http", 4) == 0;
    trace_line("image: found by %s, text moved by 0x%08x, data by 0x%08x, the data %s", sHow,
               sText, sData, data ? "checks out" : "does not check out");
    DEBUG_FUNCTION_LINE("WiiULeanback: image found by %s, text +0x%08x, data +0x%08x", sHow,
                        sText, sData);
    return true;
}
