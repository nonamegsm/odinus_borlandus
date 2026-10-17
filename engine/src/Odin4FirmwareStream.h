#ifndef EASYJTAG_ODIN4_FIRMWARE_STREAM_H
#define EASYJTAG_ODIN4_FIRMWARE_STREAM_H
#include "Odin4UsbTransport.h"
#include "Odin4DeviceInfo.h"
#include "BccCompat.h"
#include <memory>
#include <cstring>
#include <string>
#include <sstream>
#include <vector>
namespace ejodin4 {
class ImageReader {
public:
    virtual ~ImageReader() {}
    // OK with actual == 0 means validated EOF. Never expands Android sparse data.
    virtual EJODIN4_Status read(uint8_t* data, uint32_t capacity, uint32_t& actual,
        const Cancellation& cancel, std::string& error) = 0;
    // Actual stored input consumed, excluding prefetched but undecoded bytes.
    virtual uint64_t stored_bytes_processed() const { return 0; }
};
class EncodedImageReader {
public:
    virtual ~EncodedImageReader() {}
    // Returns original LZ4 block records only. OK with zero decoded bytes is validated EOF.
    virtual EJODIN4_Status read_sequence(uint32_t decoded_limit, std::vector<uint8_t>& records,
        uint32_t& decoded_bytes, const Cancellation& cancel, std::string& error) = 0;
};
// A short, indexed label preserves verification identity without truncating
// long archive paths into identical progress names or splitting UTF-8 bytes.
inline std::string verification_member_name(const char* stage,size_t member,const std::string& path) {
    const size_t slash=path.find_last_of("/\\");
    std::string name=path.substr(slash==std::string::npos?0:slash+1);
    if(name.size()>160){size_t cut=160;while(cut&&((static_cast<unsigned char>(name[cut])&0xc0)==0x80))--cut;name.resize(cut);}
    std::ostringstream text;text<<stage<<" "<<(member+1)<<": "<<name;return text.str();
}
struct VerificationProgress {
    uint32_t phase,role,archive_index,archive_count;
    VerificationProgress():phase(0),role(0),archive_index(0),archive_count(0),completed_bytes(0),total_bytes(0){}
    std::string name;
    uint64_t completed_bytes,total_bytes;
};
typedef bcc::Callback1<const VerificationProgress&> VerificationCallback;
typedef bcc::Callback2<uint64_t,uint64_t> ArchiveScanCallback;
struct PlanImage {
    uint32_t role;
    std::string name;
    uint64_t source_offset, stored_bytes, decoded_bytes, expanded_bytes;
    bcc::Array<uint8_t,32> digest;
    std::string sha256;
    EJODIN4_PitEntry route;
    bool large_partition;
    size_t archive_index, member_index;
    PlanImage():role(0),source_offset(0),stored_bytes(0),decoded_bytes(0),expanded_bytes(0),large_partition(false),archive_index(0),member_index(0){std::memset(&route,0,sizeof(route));}
};
struct PlanSkip { uint32_t role,reason; std::string name; EJODIN4_PitEntry route; PlanSkip(uint32_t r=0,uint32_t s=0,const std::string& n=std::string()):role(r),reason(s),name(n){std::memset(&route,0,sizeof(route));} PlanSkip(uint32_t r,uint32_t s,const std::string& n,const EJODIN4_PitEntry& p):role(r),reason(s),name(n),route(p){} };
// Private strict inspector seam. Factory keeps its deny-write/delete file handle
// alive; readers borrow it, so destroy readers before their owning archive/set.
class HeldArchive {
public:
    virtual ~HeldArchive() {}
    std::vector<EJODIN4_ArchiveEntry> entries;
    EJODIN4_ArchiveSummary summary;
    HeldArchive(){std::memset(&summary,0,sizeof(summary));}
    virtual EJODIN4_Status verify_unchanged(std::string& error) const = 0;
    virtual EJODIN4_Status open_stored_member(size_t, std::unique_ptr<ImageReader>&,std::string& error) {
        error="Stored archive access is unavailable";return EJODIN4_DRIVER_UNSUPPORTED;
    }
    virtual EJODIN4_Status open_member(size_t index, std::unique_ptr<ImageReader>& reader,
        std::string& error) = 0;
    virtual EJODIN4_Status open_encoded_member(size_t, const PlanImage&,
        std::unique_ptr<EncodedImageReader>& reader, std::string& error,
        const Cancellation& = Cancellation(), const ArchiveScanCallback& = ArchiveScanCallback()) {
        reader.reset(); error="Archive has no native encoded reader"; return EJODIN4_DRIVER_UNSUPPORTED;
    }
};
EJODIN4_Status open_held_archive(const EJODIN4_FirmwareFile& file,
    const Cancellation& cancel, std::unique_ptr<HeldArchive>& output, std::string& error,
    const ArchiveScanCallback& progress = ArchiveScanCallback());
class FirmwareSet {
public:
    uint32_t data_policy;
    FirmwareSet():data_policy(0),total_bytes(0){}
    ~FirmwareSet();
    // One entry per transfer in canonical slot/member order, including
    // identical repeated destinations. Totals count every retained transfer.
    std::vector<PlanImage> images;
    std::vector<PlanSkip> skips;
    std::vector<EJODIN4_PitEntry> package_pit;
    std::vector<uint8_t> package_pit_bytes;
    uint64_t total_bytes;
    EJODIN4_Status verify_unchanged(std::string& error) const;
    EJODIN4_Status validate_device_pit(const std::vector<EJODIN4_PitEntry>& pit,
        uint32_t verified_sector_bytes, std::string& error,
        uint64_t verified_userdata_capacity_bytes = 0);
    EJODIN4_Status authenticate_identity(FirmwareIdentity& identity,const Cancellation& cancel,
        const VerificationCallback& progress = VerificationCallback());
    EJODIN4_Status open_image(size_t index, std::unique_ptr<ImageReader>& reader,
        std::string& error);
    EJODIN4_Status open_encoded_image(size_t index, std::unique_ptr<EncodedImageReader>& reader,
        std::string& error, const Cancellation& cancel = Cancellation(), const ArchiveScanCallback& progress = ArchiveScanCallback());
    std::vector<HeldArchive*> archives;
private:
    FirmwareSet(const FirmwareSet&);
    FirmwareSet& operator=(const FirmwareSet&);
};
EJODIN4_Status prepare_firmware_set(const EJODIN4_FirmwareFile* files, uint32_t count,
    uint32_t policy, uint64_t decoded_limit, const Cancellation& cancel,
    std::unique_ptr<FirmwareSet>& output, std::string& error,
    const VerificationCallback& progress = VerificationCallback());
// Incremental Android sparse geometry verifier, also used by synthetic tests.
class SparseGeometry {
public:
    SparseGeometry();
    ~SparseGeometry();
    EJODIN4_Status consume(const uint8_t* data, size_t size, std::string& error);
    EJODIN4_Status finish(uint64_t decoded_bytes, uint64_t& expanded_bytes,
        std::string& error);
private:
    SparseGeometry(const SparseGeometry&);
    SparseGeometry& operator=(const SparseGeometry&);
    // Classic BCC instantiates unique_ptr's deleter in consumer translation
    // units even with an out-of-line destructor; keep this pImpl owner explicit.
    struct State; State* state_;
};
}
#endif
