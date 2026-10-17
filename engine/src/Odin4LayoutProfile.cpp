#include "Odin4LayoutProfile.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>

#include <cstring>
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
#ifndef PROV_RSA_AES
#define PROV_RSA_AES 24
#endif
namespace ejodin4 {
namespace {
uint32_t le32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
struct Hash {
    HCRYPTPROV provider; HCRYPTHASH hash;
    Hash():provider(0),hash(0){}
    ~Hash() { if (hash) CryptDestroyHash(hash); if (provider) CryptReleaseContext(provider, 0); }
};
bool reviewed_package(const std::vector<uint8_t>& bytes, std::string& error) {
    // User-supplied, MD5-verified E1S_EUR_OPENX.pit from S921BXXS9BYH2.
    // This fingerprint identifies reviewed layout evidence, not firmware authenticity.
    const uint8_t expected[32]={
        0x9b,0x44,0x09,0x94,0x6e,0xa0,0xcc,0x1c,0xba,0x6d,0x94,0xd2,0xd6,0xf9,0x20,0xb1,
        0xd9,0x6e,0x70,0x8a,0x97,0x79,0x64,0x70,0x72,0xdb,0x55,0x14,0x3a,0x66,0x89,0x8e};
    if (bytes.size() != 7280) return false;
    Hash state;
    if (!CryptAcquireContextW(&state.provider, 0, 0, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
        !CryptCreateHash(state.provider, CALG_SHA_256, 0, 0, &state.hash) ||
        !CryptHashData(state.hash, const_cast<BYTE*>(&bytes[0]), static_cast<DWORD>(bytes.size()), 0)) {
        error = "Cannot fingerprint the reviewed layout"; return false;
    }
    bcc::Array<uint8_t,32> digest; DWORD size = static_cast<DWORD>(digest.size());
    if (!CryptGetHashParam(state.hash, HP_HASHVAL, digest.data(), &size, 0) || size != digest.size()) {
        error = "Cannot finish the layout fingerprint"; return false;
    }
    return std::memcmp(digest.data(),expected,32)==0;
}
bool intended_build(const EJODIN4_FirmwareFile* files, uint32_t count, bool selected = false) {
    uint32_t present = 0, roles = 0;
    const uint32_t max_role = selected ? EJODIN4_PACKAGE_PIT_REFERENCE : EJODIN4_PACKAGE_USERDATA;
    for (uint32_t i = 0; i < count; ++i) {
        const EJODIN4_FirmwareFile& file = files[i];
        if (!file.path || file.path_bytes < 2) return false;
        if (file.role < 1 || file.role > max_role ||
            (roles & (1u << file.role))) return false;
        roles |= 1u << file.role;
        std::string name(file.path, file.path_bytes - 1);
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name.erase(0, slash + 1);
        for (size_t ni=0;ni<name.size();++ni) {char& c=name[ni];if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));}
        const char* prefix = 0;
        switch (file.role) {
        case EJODIN4_PACKAGE_BL: prefix = "bl_s921bxxs9byh2_"; present |= 1; break;
        case EJODIN4_PACKAGE_AP: prefix = "ap_s921bxxs9byh2_"; present |= 2; break;
        case EJODIN4_PACKAGE_CP: prefix = "cp_s921bxxs9byh2_"; present |= 4; break;
        case EJODIN4_PACKAGE_CSC: prefix = "csc_oxm_s921boxm9byh2_"; present |= 8; break;
        case EJODIN4_PACKAGE_HOME_CSC: prefix = "home_csc_oxm_s921boxm9byh2_"; present |= 8; break;
        case EJODIN4_PACKAGE_USERDATA:
        case EJODIN4_PACKAGE_PIT_REFERENCE:
            // The optional slot may repeat a reviewed CSC package. It does not
            // provide independent build evidence or relax the layout profile.
            if (name.compare(0, std::strlen("csc_oxm_s921boxm9byh2_"), "csc_oxm_s921boxm9byh2_") == 0)
                prefix = "csc_oxm_s921boxm9byh2_";
            else prefix = "home_csc_oxm_s921boxm9byh2_";
            break;
        default: return false;
        }
        if (name.compare(0, std::strlen(prefix), prefix)) return false;
    }
    if (!selected) return present == 15;
    return (roles & ((1u << EJODIN4_PACKAGE_PIT_REFERENCE) - 2u)) != 0 &&
        !((roles & (1u << EJODIN4_PACKAGE_CSC)) && (roles & (1u << EJODIN4_PACKAGE_HOME_CSC)));
}
bool protected_alias(const char* field) {
    std::string name;
    size_t length = 0; while (length < 32 && field[length]) ++length;
    name.assign(field, length);
    for (size_t ni=0;ni<name.size();++ni) {char& c=name[ni];if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));}
    const size_t dot = name.find('.'); if (dot != std::string::npos) name.resize(dot);
    return name == "userdata" || name == "user_data" || name == "misc" || name == "omr";
}
bool exact_anchors(const FirmwareSet& firmware, bool selected = false) {
    const char* const names[] = {"SUPER", "BOOT", "CACHE", "PRISM", "OPTICS"};
    uint32_t found = 0;
    bcc::Array<const PlanImage*,5> first_anchor;
    bool reviewed_mount_configuration = false;
    for (size_t im=0;im<firmware.images.size();++im) {
        const PlanImage& image=firmware.images[im];
        if (selected && (image.role < EJODIN4_PACKAGE_BL || image.role > EJODIN4_PACKAGE_USERDATA ||
            protected_alias(image.route.partition_name) || protected_alias(image.route.file_name) ||
            protected_alias(image.route.fota_name))) return false;
        if (std::strncmp(image.route.partition_name, "VENDOR_BOOT", 32) == 0) {
            if (image.sha256 != "9fb4b7b4c0e132d9a95cca9641c8f34434612838e6427eb5e04f35077c2a975d") return false;
            reviewed_mount_configuration = true;
        }
        for (uint32_t i = 0; i < 5; ++i) {
            const size_t size = std::strlen(names[i]);
            if (std::memcmp(image.route.partition_name, names[i], size) || image.route.partition_name[size] != 0) continue;
            if (image.route.device_type != 8 || !image.route.block_count ||
                image.expanded_bytes != static_cast<uint64_t>(image.route.block_count) * 4096) return false;
            if (first_anchor[i]) {
                const PlanImage& previous = *first_anchor[i];
                if (std::memcmp(&previous.route, &image.route, sizeof(image.route))) return false;
            } else first_anchor[i] = &image;
            found |= 1u << i;
        }
    }
    return selected ? !firmware.images.empty() : found == 31 && reviewed_mount_configuration;
}
}
bool same_pit_layout(const std::vector<uint8_t>& package, const std::vector<uint8_t>& live) {
    if (package.size() < 28 || live.size() < 28 ||
        le32(&package[0]) != 0x12349876 || le32(&live[0]) != 0x12349876) return false;
    const uint32_t count = le32(&package[0] + 4);
    if (!count || count > 512 || count != le32(&live[0] + 4)) return false;
    const size_t bytes = 28u + static_cast<size_t>(count) * sizeof(EJODIN4_PitEntry);
    return package.size() >= bytes && live.size() >= bytes && std::memcmp(&package[0], &live[0], bytes) == 0;
}
EJODIN4_Status resolve_layout_profile(const FirmwareSet& firmware,
    const std::vector<uint8_t>& live_pit, const EJODIN4_FirmwareFile* files,
    uint32_t count, uint32_t caller_sector_bytes, LayoutResolution& output, std::string& error) {
    output = LayoutResolution();
    if (caller_sector_bytes) {
        if (caller_sector_bytes != 512 && caller_sector_bytes != 1024 && caller_sector_bytes != 2048 && caller_sector_bytes != 4096) {
            error = "Caller geometry unit is unsupported"; return EJODIN4_INVALID_ARGUMENT;
        }
        output.sector_bytes = caller_sector_bytes; output.source = "Caller-verified geometry";
        return EJODIN4_OK;
    }
    const bool selected = firmware.data_policy == EJODIN4_UPDATE_SELECTED;
    if (!files || !count || count > 6 || !intended_build(files, count, selected) ||
        !same_pit_layout(firmware.package_pit_bytes, live_pit) ||
        !reviewed_package(firmware.package_pit_bytes, error) || !exact_anchors(firmware, selected)) {
        if (error.empty()) error = "No reviewed matching device layout; verified device geometry is required before image writes";
        return EJODIN4_PIT_MAPPING_ERROR;
    }
    output.sector_bytes = 4096;
    if (selected) {
        output.source = "Matched SM-S921B BYH2 package and live PIT: 4096-byte units for selected slots; no USERDATA capacity inference";
        return EJODIN4_OK;
    }
    output.source = "Matched SM-S921B BYH2 factory layout: 4096-byte units; USERDATA minimum 64 GiB (profile inference, not measured capacity)";
    // Samsung specifies 103.9GB user-available storage for the minimum 128GB
    // SM-S921B variant. The pinned vendor_boot fstab mounts USERDATA at /data.
    // This deliberately smaller floor assumes that exact stock layout; it is
    // neither READ CAPACITY nor a claim about modified storage/GPT tails.
    // See docs/odin4-windows-port.md for source/variant and firmware evidence.
    output.userdata_lower_bound_bytes = (uint64_t(64)*1024*1024*1024);
    return EJODIN4_OK;
}
}
