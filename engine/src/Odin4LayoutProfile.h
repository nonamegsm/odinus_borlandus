#ifndef EASYJTAG_ODIN4_LAYOUT_PROFILE_H
#define EASYJTAG_ODIN4_LAYOUT_PROFILE_H
#include "Odin4FirmwareStream.h"
namespace ejodin4 {
struct LayoutResolution {
    uint32_t sector_bytes;
    bool per_partition_units;
    const char* source;
    LayoutResolution():sector_bytes(0),per_partition_units(false),source(0){}
};
// Compare logical header/entry bytes; transport padding is not layout identity.
bool same_pit_layout(const std::vector<uint8_t>& package,const std::vector<uint8_t>& live);
// A positive caller unit overrides all routes. Otherwise selected type-2 MMC
// entries use512-byte units and type-8 UFS entries use4096-byte units.
EJODIN4_Status resolve_layout_profile(const FirmwareSet& firmware,
    const std::vector<uint8_t>& live_pit,const EJODIN4_FirmwareFile* files,
    uint32_t count,uint32_t caller_sector_bytes,LayoutResolution& result,
    std::string& error);
}
#endif
