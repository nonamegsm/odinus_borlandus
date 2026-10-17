#ifndef EASYJTAG_ODIN4_FIRMWARE_IDENTITY_H
#define EASYJTAG_ODIN4_FIRMWARE_IDENTITY_H
#include "EasyJtagOdin4Core.h"
#include <cstddef>
#include <map>
#include <string>
#include <vector>
namespace ejodin4 {
struct SignedFirmwareManifest {
    std::string model, blocked_carriers;
    std::map<std::string,std::string> member_sha256;
};
// The 256-byte, big-endian RSA signature precedes the exact authenticated text.
// Production trust is limited to the two public keys embedded in native Odin.
EJODIN4_Status verify_signed_manifest(const std::vector<uint8_t>& bytes,
    SignedFirmwareManifest& output, std::string& error);
namespace firmware_identity_detail {
// Internal test seams, not exported through the engine ABI. Parsing alone does
// not establish authenticity and must never authorize a production transfer.
EJODIN4_Status parse_authenticated_manifest(const std::string& payload,
    SignedFirmwareManifest& output, std::string& error);
EJODIN4_Status verify_rsa_pkcs1_sha256(const std::vector<uint8_t>& public_key_blob,
    const uint8_t* signature_be, size_t signature_bytes, const uint8_t* payload,
    size_t payload_bytes, std::string& error);
}
}
#endif
