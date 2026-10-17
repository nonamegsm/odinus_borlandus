#ifndef EASYJTAG_ODIN4_LAYOUT_PROFILE_H
#define EASYJTAG_ODIN4_LAYOUT_PROFILE_H
#include "Odin4FirmwareStream.h"
namespace ejodin4 {
struct LayoutResolution {
    uint32_t sector_bytes;
    uint64_t userdata_lower_bound_bytes;
    const char* source;
    LayoutResolution():sector_bytes(0),userdata_lower_bound_bytes(0),source(0){}
};
// Raw header and all entry bytes are compared; opaque suffix is not authenticated.
bool same_pit_layout(const std::vector<uint8_t>& package, const std::vector<uint8_t>& live);
// A caller-provided unit is distinct from the single reviewed layout inference.
EJODIN4_Status resolve_layout_profile(const FirmwareSet& firmware,
    const std::vector<uint8_t>& live_pit, const EJODIN4_FirmwareFile* files,
    uint32_t count, uint32_t caller_sector_bytes, LayoutResolution& result,
    std::string& error);
}
#endif
