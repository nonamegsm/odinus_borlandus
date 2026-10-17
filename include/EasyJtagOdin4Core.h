/* Native classic BCC Odin engine ABI 1.11. Packed records retained for checked interfaces. */
#ifndef EASYJTAG_ODIN4_CORE_H
#define EASYJTAG_ODIN4_CORE_H

#include <stdint.h>

#define EJODIN4_CALL __cdecl
#define EJODIN4_API

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t EJODIN4_Status;
enum {
    EJODIN4_OK = 0,
    EJODIN4_INVALID_ARGUMENT = 1,
    EJODIN4_INVALID_PIT = 2,
    EJODIN4_BUFFER_TOO_SMALL = 3,
    EJODIN4_INVALID_RESPONSE = 4,
    EJODIN4_TRAILER_NOT_FOUND = 5,
    EJODIN4_INTERNAL_ERROR = 6,
    EJODIN4_DRIVER_UNSUPPORTED = 7,
    EJODIN4_NO_DEVICE = 8,
    EJODIN4_TIMEOUT = 9,
    EJODIN4_CANCELLED = 10,
    EJODIN4_USB_IO_ERROR = 11,
    EJODIN4_BAD_HANDSHAKE = 12,
    EJODIN4_FILE_IO_ERROR = 13,
    EJODIN4_INVALID_ARCHIVE = 14,
    EJODIN4_CHECKSUM_MISMATCH = 15,
    EJODIN4_UNSUPPORTED_ARCHIVE = 16,
    EJODIN4_INVALID_COMPRESSED_DATA = 17,
    EJODIN4_DECODE_LIMIT_EXCEEDED = 18,
    EJODIN4_PIT_MAPPING_ERROR = 19
};

/* Called synchronously on the calling thread. Return nonzero to cancel.
 * Callback must not throw or reenter the operation. It may be NULL. */
typedef int32_t (EJODIN4_CALL *EJODIN4_CancelCallback)(void* context);
enum { EJODIN4_USB_WINUSB_COMPATIBLE = 1 };
enum { EJODIN4_DOWNLOAD_WINUSB = 1, EJODIN4_DOWNLOAD_SAMSUNG_SERIAL = 2 };

enum { EJODIN4_ARCHIVE_ENTRY_LZ4 = 1, EJODIN4_ARCHIVE_ENTRY_PIT = 2 };
enum { EJODIN4_ARCHIVE_MD5_PRESENT = 1, EJODIN4_ARCHIVE_MD5_VERIFIED = 2 };
enum { EJODIN4_IMAGE_LZ4 = 1, EJODIN4_IMAGE_PIT = 2, EJODIN4_IMAGE_MAPPED = 4 };
enum { EJODIN4_ANALYSIS_PIT_AVAILABLE = 1, EJODIN4_ANALYSIS_ALL_IMAGES_MAPPED = 2, EJODIN4_ANALYSIS_EXTERNAL_PIT = 4 };


/* Flash operations are explicit. PLAN_ONLY performs no device I/O; READ_PIT
 * reads a session PIT and closes without image transfer/reboot. WRITE requires
 * an explicit data policy; package PIT is never uploaded or repartitioned. */
enum { EJODIN4_FLASH_PLAN_ONLY = 0, EJODIN4_FLASH_READ_PIT = 1, EJODIN4_FLASH_WRITE = 2 };
enum { EJODIN4_PACKAGE_BL = 1, EJODIN4_PACKAGE_AP = 2, EJODIN4_PACKAGE_CP = 3, EJODIN4_PACKAGE_CSC = 4, EJODIN4_PACKAGE_HOME_CSC = 5, EJODIN4_PACKAGE_USERDATA = 6, EJODIN4_PACKAGE_PIT_REFERENCE = 7 };
/* Package order is BL, AP, CP, selected CSC variant, optional USERDATA slot.
 * HOME_CSC and USERDATA are archive roles, not image names. Preserve mode may
 * use CSC + a USERDATA-slot OEM download list; an additional CSC may supply
 * the reference PIT for a selected HOME_CSC. Identical repeated writes remain
 * ordered transfers with independent payloads; conflicting PIT routes fail. */
/* Selected update permits any nonempty subset of archive slots, plus one
 * optional PIT_REFERENCE archive. Reference images never become transfers.
 * USERDATA, MISC and OMR destinations are always excluded in selected mode. */
enum { EJODIN4_PRESERVE_DATA = 1, EJODIN4_CLEAN_INSTALL = 2, EJODIN4_UPDATE_SELECTED = 3 };
enum { EJODIN4_FLASH_REBOOT = 1 };
enum { EJODIN4_FLASH_ANY_WRITE = 1, EJODIN4_FLASH_FINALIZED = 2, EJODIN4_FLASH_REBOOT_SENT = 4, EJODIN4_FLASH_PIT_MATCHED = 8, EJODIN4_FLASH_PLAN_READY = 16, EJODIN4_FLASH_PER_PARTITION_PIT_UNITS = 32 };
enum {
    EJODIN4_PHASE_PREFLIGHT = 1, EJODIN4_PHASE_PLAN_IMAGE = 2,
    EJODIN4_PHASE_CONNECTING = 3, EJODIN4_PHASE_HANDSHAKE = 4,
    EJODIN4_PHASE_NEGOTIATING = 5, EJODIN4_PHASE_PIT_ENTRY = 6,
    EJODIN4_PHASE_PIT_READY = 7, EJODIN4_PHASE_PLAN_VALIDATED = 8,
    EJODIN4_PHASE_IMAGE_BEGIN = 9, EJODIN4_PHASE_DATA_ACK = 10,
    EJODIN4_PHASE_IMAGE_COMPLETE = 11, EJODIN4_PHASE_FINALIZING = 12,
    EJODIN4_PHASE_REBOOT = 13, EJODIN4_PHASE_COMPLETE = 14,
    EJODIN4_PHASE_FAILED = 15, EJODIN4_PHASE_CANCELLED = 16,
    EJODIN4_PHASE_PLAN_SKIP = 17,
    EJODIN4_PHASE_VERIFY_ARCHIVE = 20, EJODIN4_PHASE_VERIFY_IMAGE = 21
};
enum { EJODIN4_SKIP_METADATA = 1, EJODIN4_SKIP_PIT = 2, EJODIN4_SKIP_USERDATA = 3, EJODIN4_SKIP_IDENTICAL_DUPLICATE = 4, EJODIN4_SKIP_CSC_ALTERNATIVE = 5, EJODIN4_SKIP_DOWNLOAD_LIST = 6, EJODIN4_SKIP_PIT_REFERENCE = 7 };

#pragma pack(push, 1)
typedef struct EJODIN4_AnalyzedImage {
    char name[256];
    uint64_t stored_bytes;
    uint64_t decoded_bytes;
    uint32_t flags;
    uint32_t pit_identifier;
    uint32_t binary_type;
    uint32_t device_type;
    char partition_name[32];
} EJODIN4_AnalyzedImage;

typedef struct EJODIN4_AnalysisSummary {
    uint32_t entry_count;
    uint32_t pit_entry_count;
    uint32_t flags;
    uint64_t decoded_bytes;
} EJODIN4_AnalysisSummary;

typedef struct EJODIN4_ArchiveEntry {
    char name[256];
    uint64_t data_offset;
    uint64_t data_bytes;
    uint32_t flags;
} EJODIN4_ArchiveEntry;

typedef struct EJODIN4_ArchiveSummary {
    uint64_t archive_bytes;
    uint64_t payload_bytes;
    uint64_t image_bytes;
    uint32_t entry_count;
    uint32_t flags;
    char actual_md5[33];
    char expected_md5[33];
} EJODIN4_ArchiveSummary;
typedef struct EJODIN4_UsbDevice {
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t flags;
    char device_path[1024];
    char instance_id[256];
    char description[256];
    char driver[64];
} EJODIN4_UsbDevice;

typedef struct EJODIN4_UsbProbe {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t interface_number;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint8_t reserved0;
    uint16_t bulk_in_packet_size;
    uint16_t bulk_out_packet_size;
    uint8_t handshake_complete;
    uint8_t reserved1;
} EJODIN4_UsbProbe;

typedef struct EJODIN4_DownloadProbe {
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t transport;
    uint8_t handshake_complete;
    uint8_t reserved[3];
} EJODIN4_DownloadProbe;

typedef struct EJODIN4_PitEntry {
    uint32_t binary_type;
    uint32_t device_type;
    uint32_t identifier;
    uint32_t attributes;
    uint32_t update_attributes;
    uint32_t block_size_or_offset;
    uint32_t block_count;
    uint32_t file_offset;
    uint32_t file_size;
    char partition_name[32];
    char file_name[32];
    char fota_name[32];
} EJODIN4_PitEntry;


/* Pointer-bearing request records are native Windows x86 layouts. All pointed
 * data remains caller-owned and valid until the synchronous call returns. */
typedef struct EJODIN4_FirmwareFile {
    uint32_t role;
    const char* path;
    uint32_t path_bytes;
} EJODIN4_FirmwareFile;

typedef struct EJODIN4_FlashRequest {
    uint32_t struct_size;
    uint32_t operation;
    uint32_t data_policy;
    uint32_t flags;
    const char* device_path;
    uint32_t device_path_bytes;
    const EJODIN4_FirmwareFile* files;
    uint32_t file_count;
    uint64_t decoded_limit;
    uint32_t io_timeout_ms;
    uint32_t logical_sector_bytes; /* Zero selects per-partition Samsung PIT allocation units. */
    uint64_t userdata_capacity_bytes;
    uint32_t reserved[2];
} EJODIN4_FlashRequest;

typedef struct EJODIN4_FlashProgress {
    uint32_t struct_size;
    uint32_t phase;
    uint32_t image_index;
    uint32_t image_count;
    uint32_t package_role;
    uint64_t acknowledged_bytes;
    uint64_t total_bytes;
    uint64_t image_acknowledged_bytes;
    uint64_t image_bytes;
    uint64_t source_offset;
    uint64_t stored_bytes;
    uint64_t expanded_bytes;
    EJODIN4_PitEntry partition;
    /* Member basename on image events; geometry source label on PLAN_VALIDATED. */
    char image_name[256];
    char sha256[65];
    uint32_t skip_reason;
    uint32_t reserved;
} EJODIN4_FlashProgress;
/* VERIFY_ARCHIVE reports real hashed TAR payload bytes; VERIFY_IMAGE reports
 * stored member bytes consumed by decoding/SHA validation, excluding prefetch.
 * source_offset/stored_bytes are completed/total for that verification stage;
 * image_index/image_count identify canonical archive ordinal/count, package_role
 * identifies its slot, and image_name is archive basename/member name. All ACK,
 * transfer-total/image-byte fields and reserved remain zero. Start/end events
 * are forced; intermediate updates are throttled to at most four per second.
 * A stage completion does not assert firmware readiness or any device writes. */

typedef struct EJODIN4_FlashResult {
    uint32_t struct_size;
    uint32_t operation;
    uint32_t phase;
    uint32_t protocol_version;
    uint32_t transport;
    uint32_t image_count;
    uint32_t images_completed;
    uint32_t pit_entry_count;
    uint32_t flags;
    uint64_t total_bytes;
    uint64_t acknowledged_bytes;
    uint32_t last_response_id;
    uint32_t last_ack;
    uint32_t last_proven_phase;
    uint32_t logical_sector_bytes;
} EJODIN4_FlashResult;

typedef struct EJODIN4_Response {
    int32_t id;
    int32_t ack;
} EJODIN4_Response;

typedef struct EJODIN4_TrailerPosition {
    int64_t md5_offset;
    int64_t trailer_start;
} EJODIN4_TrailerPosition;
#pragma pack(pop)

/* Version is major << 16 | minor. Diagnostics are optional, bounded and NUL
 * terminated when capacity > 0. A NULL diagnostic requires capacity == 0.
 * Caller buffers must be valid for the stated capacity and must not overlap. */
EJODIN4_API uint32_t EJODIN4_CALL EJODIN4_GetAbiVersion(void);

/* PIT input limit: 1 MiB. Uses upstream's 1..512 entry validation, including
 * nonzero unique identifiers and nonempty partition names. Numeric fields are
 * host endian; name arrays are raw 32-byte fields and may have no terminator.
 * NULL entries with capacity 0 queries required count: BUFFER_TOO_SMALL.
 * A valid insufficient-capacity call returns required count without writing
 * entries. Other failures set count to 0. Trailing PIT bytes are accepted. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_ParsePit(
    const uint8_t* bytes, uint32_t byte_count,
    EJODIN4_PitEntry* entries, uint32_t entry_capacity, uint32_t* out_count,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Writes exactly 1024 little-endian wire bytes, leaving extra capacity alone.
 * At most 9 integers and 128 raw character bytes; excess is rejected rather
 * than truncated. Command/parameter values are passed through unchanged.
 * NULL input arrays are permitted only with zero count. No output on error. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_BuildRequest(
    int32_t command, int32_t parameter,
    const int32_t* integers, uint32_t integer_count,
    const uint8_t* characters, uint32_t character_count,
    uint8_t* output, uint32_t output_capacity,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Exactly 8 wire bytes required. Checks expected id and nonnegative ACK using
 * upstream validation. Output is host endian and unchanged on failure. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_DecodeResponse(
    const uint8_t* bytes, uint32_t byte_count, int32_t expected_id,
    EJODIN4_Response* output, char* diagnostic, uint32_t diagnostic_capacity);

/* Tail contains at most the final 64 KiB of a file. Returns absolute offsets
 * using upstream trailer-location heuristics, not MD5 verification. Offset +
 * byte_count must fit int64_t. On failure both output offsets are -1. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_LocateTarMd5Trailer(
    const uint8_t* tail, uint32_t byte_count, uint64_t tail_offset,
    EJODIN4_TrailerPosition* output,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Enumerates only present known Samsung Download-mode USB identities.
 * WinUSB-compatible records carry flag 1 and an exact interface path. Others
 * carry flags 0 and an empty path; driver is the registered service name.
 * UTF-8 strings are NUL terminated. Identity overflow fails, never truncates.
 * NULL devices/capacity 0 queries count. Insufficient capacity returns required
 * count without partial output. Other errors reset count to 0. Enumeration
 * performs no device opens or driver binding changes. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_EnumerateUsbDevices(
    EJODIN4_UsbDevice* devices, uint32_t capacity, uint32_t* count,
    EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* path_bytes includes the sole trailing NUL (2..1024); path must be valid
 * UTF-8, timeout_ms 1..30000. Re-enumerates and opens only an exact compatible
 * selected identity, selects an unambiguous bulk pair (CDC data preferred),
 * writes exactly four ASCII ODIN bytes, reads <=512 bytes and requires LOKE
 * prefix. Does not begin a session, query PIT, reboot or flash. Output remains
 * unchanged on failure. Calls are independent; caller must serialize access to
 * the same device. Cancellation/deadline checked between native calls and
 * while pending I/O. Native setup and faulty-driver cancellation/drain can
 * exceed the deadline: pending I/O is always drained before buffers are freed. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_ProbeUsbDevice(
    const char* device_path, uint32_t path_bytes, uint32_t timeout_ms,
    EJODIN4_UsbProbe* output, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Combined Download enumeration. Same output/query rules as EnumerateUsbDevices.
 * flags are exactly DOWNLOAD_WINUSB or DOWNLOAD_SAMSUNG_SERIAL for usable
 * interfaces, zero for unsupported records. Serial records are present Samsung
 * modem-class children with a valid COM port and exact modem interface path.
 * The COM name is descriptive only; device_path is always the selected native
 * interface identity. No device handles are opened or drivers changed. The old
 * EnumerateUsbDevices API remains WinUSB-only, including its flags 0/1 contract. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_EnumerateDownloadDevices(
    EJODIN4_UsbDevice* devices, uint32_t capacity, uint32_t* count,
    EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Same bounded path/deadline/output/cancellation rules as ProbeUsbDevice.
 * Re-enumerates and requires one exact supported selected interface. WinUSB
 * retains its existing handshake. Samsung serial preserves the driver's DCB,
 * restores communication timeouts before close, and exchanges exactly ODIN /
 * LOKE (four bytes each), allowing partial serial transfers. No AT commands,
 * line-state changes, session, reboot, PIT query or firmware writes occur.
 * Pending kernel I/O is cancelled and drained before any resource is freed. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_ProbeDownloadConnection(
    const char* device_path, uint32_t path_bytes, uint32_t timeout_ms,
    EJODIN4_DownloadProbe* output, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Read-only TAR / TAR.MD5 container preflight, not flash readiness. Path is an
 * absolute Windows drive/UNC UTF-8 file path; path_bytes includes its sole NUL
 * (2..32768). No USB, extraction, decompression or image validation occurs.
 * Strict regular-file/directory TAR subset: unsupported PAX/GNU extensions,
 * links and sparse records are rejected. At most 512 regular entries, unique
 * safe UTF-8 paths up to 255 bytes, at most 1024 total headers, two zero end
 * blocks and zero end padding. Per-entry padding bytes are not interpreted.
 * payload_bytes is the complete hashed TAR prefix (including end/padding),
 * image_bytes sums stored regular-file sizes. LZ4/PIT flags recognize names
 * only (.lz4, .pit or .pit.lz4), not content. actual_md5 always contains the
 * TAR prefix MD5. A trailer, when present, must verify; .tar.md5 requires one.
 * MD5 detects accidental corruption; it does not authenticate firmware.
 * NULL entries/capacity 0 validates the ENTIRE file, then returns required
 * count and valid summary with BUFFER_TOO_SMALL. Other errors reset count to
 * zero and preserve entries/summary. Success writes all entries and summary.
 * Cancellation is checked before opening and between bounded 64 KiB reads.
 * The open denies write/delete sharing; size/timestamp are rechecked. Native
 * filesystem calls can block beyond cancellation; caller owns serialization. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_InspectFirmwareArchive(
    const char* path, uint32_t path_bytes,
    EJODIN4_ArchiveEntry* entries, uint32_t capacity, uint32_t* count,
    EJODIN4_ArchiveSummary* summary, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Offline analysis only; does not prove device compatibility or flash readiness.
 * First performs the same strict container/MD5 validation as Inspect, retaining
 * its read-only file handle through decoding. Modern single LZ4 frames only:
 * validates checksums/content size and rejects dictionary, legacy/skippable,
 * concatenated, truncated or trailing data. Decoded images are discarded.
 * decoded_limit is 1..1 TiB and bounds the sum including plain/PIT members.
 * Optional external PIT is NULL/0 or an empty NUL string, otherwise an absolute
 * path under the same rules. It takes precedence over an embedded PIT; without
 * it multiple embedded PIT members are ambiguous. Only selected PIT data is
 * retained (at most 1 MiB); existing ParsePit restrictions apply.
 * Mapping uses only ASCII-folded image basename minus one .lz4 suffix versus
 * exact PIT file_name. No aliases or partition capacity guesses. Duplicate PIT
 * names/destinations fail. PIT members themselves remain unmapped; unmapped
 * identifier is UINT32_MAX and other metadata is zero. Mapped partition_name
 * is nonempty terminated UTF-8. ALL_IMAGES_MAPPED requires a non-PIT image.
 * NULL images/capacity0 fully validates before BUFFER_TOO_SMALL with valid
 * count/summary. Other failures reset count0 and preserve all outputs. No USB,
 * extraction, partition writes, driver operations or persistent side effects. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_AnalyzeFirmwareArchive(
    const char* path, uint32_t path_bytes,
    const char* optional_pit_path, uint32_t pit_path_bytes, uint64_t decoded_limit,
    EJODIN4_AnalyzedImage* images, uint32_t capacity, uint32_t* count,
    EJODIN4_AnalysisSummary* summary, EJODIN4_CancelCallback cancel, void* context,
    char* diagnostic, uint32_t diagnostic_capacity);

/* Progress is synchronous on the caller thread and must not throw/reenter.
 * Its record is valid only during the callback. No native handle crosses ABI. */
typedef void (EJODIN4_CALL *EJODIN4_FlashProgressCallback)(
    const EJODIN4_FlashProgress* progress, void* context);

/* One owner holds archives and the exact selected connection for the operation.
 * Request struct_size must exactly match, reserved fields zero, known flags/roles,
 * at most six unique package roles and caller-owned absolute UTF-8 paths.
 * PLAN_ONLY/WRITE require explicit policy and decoded_limit 1..1TiB; READ_PIT
 * permits no files/policy and never sends image data or reboot. io_timeout_ms
 * is 0 for negotiated defaults or 1000..120000. REBOOT is WRITE-only.
 * logical_sector_bytes is an optional caller PIT allocation-unit override
 * (512/1024/2048/4096). Otherwise every selected type-2 MMC entry uses512 bytes
 * and type-8 UFS entry uses4096 bytes. All images must fit the resulting byte
 * capacity using their expanded sparse extent. Unknown device types fail.
 * userdata_capacity_bytes is independently verified capacity for zero-count
 * USERDATA; it may use automatic units and must align to that route's unit.
 * A zero-count USERDATA entry has no inferred capacity floor. Mixed selected
 * eMMC/UFS units report logical_sector_bytes=0 and, for a verified WRITE plan,
 * PER_PARTITION_PIT_UNITS. Homogeneous plans report their common512/4096 unit.
 * Explicit caller units remain strict; model/build/PIT hashes are not gates.
 * Preserve policy omits USERDATA/MISC and requires a bounded selected HOME_CSC
 * or USERDATA-slot OEM allowlist; clean uses CSC. Repeated PIT destinations
 * remain ordered writes with independent payloads, hashes and byte counts;
 * conflicting PIT routes fail.
 * Selected update accepts a nonempty subset, at most one CSC variant, and an
 * optional reference archive supplying only its validated PIT. It needs no OEM
 * download list and omits USERDATA/MISC/OMR by member and PIT destination aliases.
 * Entire selected firmware is validated before any transfer; package PIT never
 * uploads. WRITE compares current device PIT and rejects unsupported routes.
 * Samsung stock-driver sessions support negotiated versions 1 through 5;
 * versions 4/5 require validated DEVINFO and manufacturer-signed authenticate.mf
 * with matching stored-member digests and common model/carrier before WRITE.
 * WinUSB image transfers retain reviewed versions 1 through 3 only. Unknown
 * versions and unsupported storage routes fail before image commands.
 * Invalid request leaves result unchanged. A valid operation publishes partial
 * result even on failure: ANY_WRITE means a partition may have changed.
 * A successful WRITE requires every image ACK, session finalization and native
 * cleanup. It does not prove subsequent Android boot or data retention.
 * Cancellation cannot undo writes. Never blindly retry an interrupted flash. */
EJODIN4_API EJODIN4_Status EJODIN4_CALL EJODIN4_FlashFirmware(
    const EJODIN4_FlashRequest* request, EJODIN4_FlashResult* result,
    EJODIN4_FlashProgressCallback progress, EJODIN4_CancelCallback cancel,
    void* context, char* diagnostic, uint32_t diagnostic_capacity);

#ifdef __cplusplus
}
#endif
#endif
