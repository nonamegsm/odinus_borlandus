#include "Odin4FirmwareStream.h"
#include "Odin4FirmwareIdentity.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
// TDM-GCC 10 ships older WinCrypt declarations; this is the documented
// SHA-256 CryptoAPI ALG_ID (ALG_CLASS_HASH | ALG_TYPE_ANY | 12).
#ifndef CALG_SHA_256
#define CALG_SHA_256 0x0000800c
#endif
#include <algorithm>

#include <cstring>

#include <map>
#include <set>

namespace ejodin4 {
namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
std::string fold(std::string s) { for(size_t i=0;i<s.size();++i) {char& c=s[i];if(c>='A'&&c<='Z')c=char(c+32);}return s; }
std::string field(const char* p,size_t n) {const char* e=static_cast<const char*>(std::memchr(p,0,n));return std::string(p,e?size_t(e-p):n);}
std::string basename(std::string s) {const size_t slash=s.find_last_of('/');if(slash!=std::string::npos)s.erase(0,slash+1);s=fold(s);if(s.size()>4&&s.compare(s.size()-4,4,".lz4")==0)s.resize(s.size()-4);return s;}
EJODIN4_Status bad(std::string& error,const char* text,EJODIN4_Status code=EJODIN4_PIT_MAPPING_ERROR){error=text;return code;}
struct Hash {
    HCRYPTPROV provider;HCRYPTHASH hash;
    Hash():provider(0),hash(0){}
    ~Hash(){if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);}
    bool start(){return CryptAcquireContextW(&provider,0,0,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)&&CryptCreateHash(provider,CALG_SHA_256,0,0,&hash);}
    bool add(const uint8_t* bytes,uint32_t size){return CryptHashData(hash,const_cast<BYTE*>(bytes),size,0)!=0;}
    bool finish(PlanImage& image){DWORD size=32;if(!CryptGetHashParam(hash,HP_HASHVAL,image.digest.data(),&size,0)||size!=32)return false;const char hex[]="0123456789abcdef";for(size_t i=0;i<image.digest.size();++i){uint8_t b=image.digest[i];image.sha256+=hex[b>>4];image.sha256+=hex[b&15];}return true;}
};
class VerifiedImageReader : public ImageReader {
    std::unique_ptr<ImageReader> source_;
    Hash hash_;
    bcc::Array<uint8_t,32> expected_;
    uint64_t bytes_, expected_bytes_;
    bool ready_, complete_;
public:
    VerifiedImageReader(ImageReader* source,const PlanImage& image)
        : source_(source),expected_(image.digest),bytes_(0),expected_bytes_(image.decoded_bytes),ready_(hash_.start()),complete_(false) {}
    bool ready() const { return ready_; }
    EJODIN4_Status read(uint8_t* data,uint32_t capacity,uint32_t& actual,const Cancellation& cancel,std::string& error) {
        actual=0;if(cancel.requested())return bad(error,"Image second-pass cancelled",EJODIN4_CANCELLED);if(complete_)return EJODIN4_OK;
        uint32_t count=0;const EJODIN4_Status status=source_->read(data,capacity,count,cancel,error);if(status!=EJODIN4_OK)return status;
        if(count>capacity||count>expected_bytes_-bytes_)return bad(error,"Image second-pass length differs from its planned transfer",EJODIN4_CHECKSUM_MISMATCH);
        if(count){if(!hash_.add(data,count))return bad(error,"Image second-pass SHA256 update failed",EJODIN4_INTERNAL_ERROR);bytes_+=count;actual=count;return EJODIN4_OK;}
        PlanImage observed;
        if(bytes_!=expected_bytes_)return bad(error,"Image second-pass ended before its planned transfer length",EJODIN4_CHECKSUM_MISMATCH);
        if(!hash_.finish(observed))return bad(error,"Image second-pass SHA256 completion failed",EJODIN4_INTERNAL_ERROR);
        if(observed.digest!=expected_)return bad(error,"Image second-pass SHA256 differs from its planned transfer",EJODIN4_CHECKSUM_MISMATCH);
        complete_=true;return EJODIN4_OK;
    }
};
bool same_route(const EJODIN4_PitEntry& a,const EJODIN4_PitEntry& b) {
    return a.identifier==b.identifier&&a.binary_type==b.binary_type&&a.device_type==b.device_type&&
        a.attributes==b.attributes&&a.update_attributes==b.update_attributes&&
        a.block_size_or_offset==b.block_size_or_offset&&a.block_count==b.block_count&&
        a.file_offset==b.file_offset&&a.file_size==b.file_size&&
        field(a.partition_name,32)==field(b.partition_name,32)&&fold(field(a.file_name,32))==fold(field(b.file_name,32))&&
        field(a.fota_name,32)==field(b.fota_name,32);
}
bool metadata(const std::string& name) {
    const std::string s=fold(name),base=basename(s);
    return s.compare(0,10,"meta-data/")==0||base=="download-list.txt"||base=="fota.zip";
}
bool misc(const PlanImage& image) {
    const std::string n=basename(image.name),partition=fold(field(image.route.partition_name,32)),file=basename(field(image.route.file_name,32));
    return partition=="misc"||n=="misc"||n=="misc.img"||n=="misc.bin"||file=="misc"||file=="misc.img"||file=="misc.bin";
}
bool userdata(const PlanImage& image) {
    const std::string n=basename(image.name),partition=fold(field(image.route.partition_name,32)),file=basename(field(image.route.file_name,32));
    return partition=="userdata"||partition=="user_data"||n=="userdata.img"||n=="userdata"||file=="userdata.img"||file=="userdata";
}
bool selected_protected_name(const std::string& name) {
    const std::string n=basename(name);
    const char* const stems[]={"userdata","user_data","misc","omr"};
    for(size_t i=0;i<4;++i){const char* stem=stems[i];
        const std::string base(stem);
        if(n==base||n==base+".img"||n==base+".bin")return true;
    }
    return false;
}
bool selected_protected(const PlanImage& image) {
    return selected_protected_name(image.name)||selected_protected_name(field(image.route.partition_name,32))||
        selected_protected_name(field(image.route.file_name,32))||selected_protected_name(field(image.route.fota_name,32));
}
class VerificationReporter {
    const VerificationCallback& callback_;
    DWORD last_;
public:
    explicit VerificationReporter(const VerificationCallback& callback):callback_(callback),last_(0){}
    void report(const VerificationProgress& progress,bool force=false){
        if(!callback_)return;
        const DWORD now=GetTickCount();
        if(!force&&DWORD(now-last_)<250)return;
        last_=now;callback_(progress);
    }
};

struct ArchiveProgressContext {
    VerificationReporter* reporter; VerificationProgress* work;
    ArchiveProgressContext(VerificationReporter* r,VerificationProgress* w):reporter(r),work(w){}
    static void report(void* context,uint64_t done,uint64_t total){ArchiveProgressContext* self=static_cast<ArchiveProgressContext*>(context);self->work->completed_bytes=done;self->work->total_bytes=total;self->reporter->report(*self->work,done==0||done==total);}
};
void report_member(VerificationReporter& reporter,VerificationProgress& work,const ImageReader& reader,uint64_t total,bool complete){work.completed_bytes=complete?total:reader.stored_bytes_processed();if(complete||work.completed_bytes<work.total_bytes)reporter.report(work,complete);}
bool unsafe_basename(const std::string& text){for(size_t i=0;i<text.size();++i){char c=text[i];if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-'))return true;}return false;}
bool control_name(const std::string& text){for(size_t i=0;i<text.size();++i){unsigned char c=static_cast<unsigned char>(text[i]);if(c<32||c==127)return true;}return false;}
std::string archive_progress_name(const EJODIN4_FirmwareFile& file){
    if(!file.path||file.path_bytes<2||file.path_bytes>32768||file.path[file.path_bytes-1])return "Selected archive";
    std::string name(file.path,file.path_bytes-1);const size_t slash=name.find_last_of("/\\");if(slash!=std::string::npos)name.erase(0,slash+1);
    return name.empty()||name.size()>255?std::string("Selected archive"):name;
}
}

struct SparseGeometry::State {
    enum Phase { Detect,FileHeader,ChunkHeader,Payload,Done,Raw,Failed } phase;
    std::vector<uint8_t> header;
    uint32_t file_header,chunk_header,block_size,total_blocks,total_chunks,chunks;
    uint64_t payload,blocks,bytes;
    uint16_t type;
    State():phase(Detect),file_header(28),chunk_header(12),block_size(0),total_blocks(0),total_chunks(0),chunks(0),payload(0),blocks(0),bytes(0),type(0){}
    EJODIN4_Status fail(std::string& error,const char* text){phase=Failed;return bad(error,text,EJODIN4_INVALID_COMPRESSED_DATA);}
    EJODIN4_Status parse_chunk(std::string& error);
};
EJODIN4_Status SparseGeometry::State::parse_chunk(std::string& error) {
        type=u16(&header[0]);const uint32_t count=u32(&header[0]+4),size=u32(&header[0]+8);
        if(size<chunk_header||count>uint64_t(total_blocks)-blocks)return fail(error,"Sparse chunk extent exceeds declared geometry");
        const uint64_t logical=uint64_t(count)*block_size;payload=uint64_t(size)-chunk_header;
        if(type==0xcac1){if(payload!=logical)return fail(error,"Sparse RAW chunk length mismatch");}
        else if(type==0xcac2){if(payload!=4||!count)return fail(error,"Sparse FILL chunk length mismatch");}
        else if(type==0xcac3){if(payload)return fail(error,"Sparse DONT_CARE chunk has payload");}
        else if(type==0xcac4){if(payload!=4||count)return fail(error,"Sparse CRC chunk shape mismatch");}
        else return fail(error,"Unknown Android sparse chunk type");
        blocks+=count;++chunks;header.clear();phase=payload?Payload:(chunks==total_chunks?Done:ChunkHeader);
        return EJODIN4_OK;
    }
SparseGeometry::SparseGeometry():state_(new State){}
SparseGeometry::~SparseGeometry(){delete state_;}
EJODIN4_Status SparseGeometry::consume(const uint8_t* data,size_t size,std::string& error){
    State& s=*state_;
    if((!data&&size)||size>UINT64_MAX-s.bytes)return s.fail(error,"Sparse input byte accounting overflows");
    s.bytes+=size;
    while(size){
        if(s.phase==State::Raw)return EJODIN4_OK;
        if(s.phase==State::Done||s.phase==State::Failed)return s.fail(error,"Trailing bytes after sparse image");
        if(s.phase==State::Payload){const size_t n=static_cast<size_t>(std::min(uint64_t(size),s.payload));data+=n;size-=n;s.payload-=n;if(!s.payload)s.phase=s.chunks==s.total_chunks?State::Done:State::ChunkHeader;continue;}
        const size_t wanted=s.phase==State::Detect?4:s.phase==State::FileHeader?s.file_header:s.chunk_header;
        const size_t n=std::min(size,wanted-s.header.size());s.header.insert(s.header.end(),data,data+n);data+=n;size-=n;
        if(s.header.size()!=wanted)continue;
        if(s.phase==State::Detect){if(u32(&s.header[0])!=0xed26ff3a){s.phase=State::Raw;continue;}s.phase=State::FileHeader;continue;}
        if(s.phase==State::FileHeader){
            if(s.header.size()==28){
                if(u16(&s.header[0]+4)!=1||u16(&s.header[0]+6)!=0)return s.fail(error,"Unsupported Android sparse version");
                s.file_header=u16(&s.header[0]+8);s.chunk_header=u16(&s.header[0]+10);s.block_size=u32(&s.header[0]+12);
                s.total_blocks=u32(&s.header[0]+16);s.total_chunks=u32(&s.header[0]+20);
                if(s.file_header<28||s.file_header>4096||s.chunk_header<12||s.chunk_header>4096||!s.block_size||s.block_size%4||!s.total_blocks||!s.total_chunks||s.total_chunks>1048576)
                    return s.fail(error,"Invalid or unbounded Android sparse geometry");
                // The package SHA256 and LZ4 checksum cover encoded bytes. A nonzero
                // expanded CRC requires an expansion-aware verifier we do not claim.
                if(u32(&s.header[0]+24))return s.fail(error,"Expanded sparse checksum is unsupported");
                if(s.header.size()!=s.file_header)continue;
            }
            s.header.clear();s.phase=State::ChunkHeader;continue;
        }
        const EJODIN4_Status status=s.parse_chunk(error);if(status!=EJODIN4_OK)return status;
    }
    return EJODIN4_OK;
}
EJODIN4_Status SparseGeometry::finish(uint64_t decoded,uint64_t& expanded,std::string& error){
    State& s=*state_;if(s.bytes!=decoded)return s.fail(error,"Sparse reader length differs from decoded byte count");
    if(s.phase==State::Raw||s.phase==State::Detect){expanded=decoded;return EJODIN4_OK;}
    if(s.phase!=State::Done||s.blocks!=s.total_blocks||s.chunks!=s.total_chunks)return s.fail(error,"Sparse image is truncated or block count differs");
    expanded=uint64_t(s.total_blocks)*s.block_size;return EJODIN4_OK;
}

FirmwareSet::~FirmwareSet(){for(size_t i=0;i<archives.size();++i)delete archives[i];}
EJODIN4_Status FirmwareSet::verify_unchanged(std::string& error)const{
    for(size_t i=0;i<archives.size();++i){const EJODIN4_Status status=archives[i]->verify_unchanged(error);if(status!=EJODIN4_OK)return status;}return EJODIN4_OK;
}
EJODIN4_Status FirmwareSet::open_encoded_image(size_t index,std::unique_ptr<EncodedImageReader>& reader,
    std::string& error,const Cancellation& cancel,const ArchiveScanCallback& progress){
    reader.reset();if(cancel.requested())return bad(error,"Encoded image audit cancelled",EJODIN4_CANCELLED);
    if(index>=images.size())return bad(error,"Invalid planned encoded image index",EJODIN4_INVALID_ARGUMENT);
    const PlanImage& image=images[index];if(image.archive_index>=archives.size())return bad(error,"Invalid planned encoded archive",EJODIN4_INTERNAL_ERROR);
    const EJODIN4_Status status=verify_unchanged(error);if(status!=EJODIN4_OK)return status;
    return archives[image.archive_index]->open_encoded_member(image.member_index,image,reader,error,cancel,progress);
}
EJODIN4_Status FirmwareSet::open_image(size_t index,std::unique_ptr<ImageReader>& reader,std::string& error){
    if(index>=images.size())return bad(error,"Invalid planned image index",EJODIN4_INVALID_ARGUMENT);
    const PlanImage& image=images[index];if(image.archive_index>=archives.size())return bad(error,"Invalid planned archive",EJODIN4_INTERNAL_ERROR);
    std::unique_ptr<ImageReader> member;
    const EJODIN4_Status status=archives[image.archive_index]->open_member(image.member_index,member,error);if(status!=EJODIN4_OK)return status;
    std::unique_ptr<VerifiedImageReader> verified(new VerifiedImageReader(member.release(),image));
    if(!verified->ready())return bad(error,"Cannot initialize image second-pass SHA256",EJODIN4_INTERNAL_ERROR);
    reader.reset(verified.release());return EJODIN4_OK;
}
static EJODIN4_Status identity_failure(FirmwareIdentity& output,EJODIN4_Status status,const std::string& reason){output.status=status;output.diagnostic=reason;return status;}
EJODIN4_Status FirmwareSet::authenticate_identity(FirmwareIdentity& output,const Cancellation& cancel,const VerificationCallback& progress){
    output=FirmwareIdentity();std::string error;FirmwareIdentity identity;
    std::set<size_t> selected;
    for(size_t n=0;n<images.size();++n)selected.insert(images[n].archive_index);
    if(selected.empty())return identity_failure(output,EJODIN4_INVALID_ARGUMENT,"No selected firmware archives for DEVINFO identity");
    std::map<size_t,size_t> manifests;
    for(std::set<size_t>::const_iterator selected_it=selected.begin();selected_it!=selected.end();++selected_it){const size_t index=*selected_it;
        if(index>=archives.size())return identity_failure(output,EJODIN4_INTERNAL_ERROR,"Selected firmware archive index is invalid");
        const std::vector<EJODIN4_ArchiveEntry>& entries=archives[index]->entries;size_t found=entries.size();
        for(size_t member=0;member<entries.size();++member){const std::string name=field(entries[member].name,256);
            if(name.compare(0,10,"meta-data/")==0&&name.size()>=14&&name.compare(name.size()-3,3,".mf")==0){
                if(found!=entries.size())return identity_failure(output,EJODIN4_UNSUPPORTED_ARCHIVE,"Ambiguous signed firmware manifests");found=member;
            }
        }
        if(found==entries.size())return identity_failure(output,EJODIN4_DRIVER_UNSUPPORTED,"Selected firmware lacks signed meta-data/*.mf identity required for native protocol 4/5");
        manifests[index]=found;
    }
    bcc::Array<uint8_t,65536> bytes;
    for(std::set<size_t>::const_iterator selected_it=selected.begin();selected_it!=selected.end();++selected_it){const size_t index=*selected_it;
        if(cancel.requested())return identity_failure(output,EJODIN4_CANCELLED,"Firmware identity authentication cancelled");
        HeldArchive& archive=*archives[index];const size_t member=manifests[index];const EJODIN4_ArchiveEntry& entry=archive.entries[member];
        if(entry.data_bytes<=256||entry.data_bytes>=131072||(entry.flags&EJODIN4_ARCHIVE_ENTRY_LZ4))return identity_failure(output,EJODIN4_INVALID_ARCHIVE,"Signed firmware manifest has invalid storage or size");
        std::unique_ptr<ImageReader> reader;EJODIN4_Status status=archive.open_stored_member(member,reader,error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);
        std::vector<uint8_t> signed_bytes;uint64_t consumed=0;
        for(;;){uint32_t actual=0;status=reader->read(bytes.data(),uint32_t(bytes.size()),actual,cancel,error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);if(!actual)break;if(actual>entry.data_bytes-consumed)return identity_failure(output,EJODIN4_INVALID_ARCHIVE,"Signed manifest extent changed");signed_bytes.insert(signed_bytes.end(),bytes.begin(),bytes.begin()+actual);consumed+=actual;}
        if(consumed!=entry.data_bytes)return identity_failure(output,EJODIN4_INVALID_ARCHIVE,"Signed manifest is truncated");
        SignedFirmwareManifest manifest;status=verify_signed_manifest(signed_bytes,manifest,error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);
        if(identity.present&&identity.model!=manifest.model)return identity_failure(output,EJODIN4_PIT_MAPPING_ERROR,"Selected authenticated firmware archives have different models");
        identity.model=manifest.model;identity.present=true;
        if(!manifest.blocked_carriers.empty()&&identity.blocked_carriers.find(manifest.blocked_carriers)==std::string::npos)identity.blocked_carriers+=manifest.blocked_carriers;
        if(identity.blocked_carriers.size()>4096)return identity_failure(output,EJODIN4_INVALID_ARCHIVE,"Merged authenticated carrier policy exceeds its bound");
        for(size_t item=0;item<archive.entries.size();++item){
            const EJODIN4_ArchiveEntry& source=archive.entries[item];const std::string name=field(source.name,256);if(name.compare(0,10,"meta-data/")==0)continue;
            const std::map<std::string,std::string>::const_iterator expected=manifest.member_sha256.find(name);if(expected==manifest.member_sha256.end())return identity_failure(output,EJODIN4_CHECKSUM_MISMATCH,"Signed manifest does not authenticate an archive member: "+name);
            status=archive.open_stored_member(item,reader,error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);Hash hash;if(!hash.start())return identity_failure(output,EJODIN4_INTERNAL_ERROR,"Cannot initialize signed member SHA-256");consumed=0;
            VerificationProgress work;work.phase=EJODIN4_PHASE_VERIFY_IMAGE;work.archive_index=uint32_t(index);work.archive_count=uint32_t(archives.size());work.name=verification_member_name("Checking signed file",item,name);work.total_bytes=source.data_bytes;
            for(size_t image_n=0;image_n<images.size();++image_n)if(images[image_n].archive_index==index){work.role=images[image_n].role;break;}
            VerificationReporter reporter(progress);reporter.report(work,true);
            for(;;){uint32_t actual=0;status=reader->read(bytes.data(),uint32_t(bytes.size()),actual,cancel,error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);if(!actual)break;if(actual>source.data_bytes-consumed||!hash.add(bytes.data(),actual))return identity_failure(output,EJODIN4_CHECKSUM_MISMATCH,"Signed member extent or hashing failed");consumed+=actual;work.completed_bytes=consumed;reporter.report(work,consumed==source.data_bytes);}
            PlanImage observed;if(consumed!=source.data_bytes||!hash.finish(observed))return identity_failure(output,EJODIN4_CHECKSUM_MISMATCH,"Signed member length or SHA-256 completion failed");
            if(observed.sha256!=expected->second)return identity_failure(output,EJODIN4_CHECKSUM_MISMATCH,"Stored archive member differs from authenticated SHA-256: "+name);
        }
    }
    EJODIN4_Status status=verify_unchanged(error);if(status!=EJODIN4_OK)return identity_failure(output,status,error);
    identity.status=EJODIN4_OK;output=identity;return EJODIN4_OK;
}
EJODIN4_Status FirmwareSet::validate_device_pit(const std::vector<EJODIN4_PitEntry>& pit,uint32_t sector,std::string& error,uint64_t verified_userdata_capacity_bytes){
    if(pit.empty()||pit.size()>512)return bad(error,"Device PIT is empty or exceeds the supported entry count");
    if(sector&&(sector<512||sector>4096||(sector&(sector-1))))return bad(error,"Caller PIT allocation unit is unsupported");
    if(verified_userdata_capacity_bytes>uint64_t(INT64_MAX))return bad(error,"Verified USERDATA capacity is outside supported bounds");
    std::map<uint32_t,const EJODIN4_PitEntry*> ids;
    for(size_t p=0;p<pit.size();++p){const EJODIN4_PitEntry& entry=pit[p];if(!entry.identifier||entry.identifier==UINT32_MAX||!ids.insert(std::make_pair(entry.identifier,&entry)).second)return bad(error,"Device PIT has invalid or duplicate destination identifiers");}
    uint32_t userdata_capacity_id=0;
    for(size_t im=0;im<images.size();++im){PlanImage& image=images[im];
        if(data_policy==EJODIN4_UPDATE_SELECTED&&selected_protected(image))return bad(error,"Selected update cannot approve USERDATA, MISC or OMR destinations");
        const std::map<uint32_t,const EJODIN4_PitEntry*>::const_iterator found=ids.find(image.route.identifier);
        if(found==ids.end()||!same_route(image.route,*found->second))return bad(error,"Package route differs from the current device PIT");
        const EJODIN4_PitEntry& entry=*found->second;
        if(entry.device_type!=2&&entry.device_type!=8)return bad(error,"Unknown partition addressing");
        const uint32_t unit=sector?sector:entry.device_type==8?4096u:512u;
        const std::string partition=fold(field(entry.partition_name,32));
        const bool is_userdata=partition=="userdata"||partition=="user_data";
        if(!image.expanded_bytes)return bad(error,"Image logical size is zero");
        uint64_t capacity=uint64_t(entry.block_count)*unit;
        if(is_userdata&&verified_userdata_capacity_bytes){
            // The scalar measurement belongs to one selected destination, not
            // every differently identified USERDATA spelling in a mixed plan.
            if(userdata_capacity_id&&userdata_capacity_id!=entry.identifier)return bad(error,"Verified USERDATA capacity is ambiguous across distinct destination identifiers");
            userdata_capacity_id=entry.identifier;
            if(verified_userdata_capacity_bytes%unit)return bad(error,"Verified USERDATA capacity must align to the partition PIT allocation unit");
            if(capacity&&capacity!=verified_userdata_capacity_bytes)return bad(error,"Verified USERDATA capacity conflicts with the positive live PIT capacity");
            capacity=verified_userdata_capacity_bytes;
        }
        if(!capacity)return bad(error,"Zero partition capacity requires verified USERDATA geometry");
        if(image.expanded_bytes>capacity)return bad(error,"Image logical size exceeds current device partition capacity");
        image.route=entry;
    }
    return verify_unchanged(error);
}

EJODIN4_Status prepare_firmware_set(const EJODIN4_FirmwareFile* files,uint32_t count,uint32_t policy,uint64_t limit,
    const Cancellation& cancel,std::unique_ptr<FirmwareSet>& output,std::string& error,const VerificationCallback& progress){
    try{
        const bool selected=policy==EJODIN4_UPDATE_SELECTED;
        if(!files||count<(selected?1u:4u)||count>6||(policy!=EJODIN4_PRESERVE_DATA&&policy!=EJODIN4_CLEAN_INSTALL&&!selected)||!limit||limit>(uint64_t(1)<<40))return bad(error,"Invalid firmware set or explicit data policy",EJODIN4_INVALID_ARGUMENT);
        std::map<uint32_t,const EJODIN4_FirmwareFile*> roles;
        const uint32_t max_role=selected?EJODIN4_PACKAGE_PIT_REFERENCE:EJODIN4_PACKAGE_USERDATA;
        for(uint32_t i=0;i<count;++i)if(files[i].role<1||files[i].role>max_role||!roles.insert(std::make_pair(files[i].role,&files[i])).second)return bad(error,"Unknown or duplicate package role",EJODIN4_INVALID_ARGUMENT);
        if(selected&&((roles.size()==1&&roles.count(EJODIN4_PACKAGE_PIT_REFERENCE))||(roles.count(EJODIN4_PACKAGE_CSC)&&roles.count(EJODIN4_PACKAGE_HOME_CSC))))return bad(error,"Selected update needs an image archive and at most one CSC variant",EJODIN4_INVALID_ARGUMENT);
        // Slot order is the public BL/AP/CP/CSC/HOME_CSC/USERDATA role order.
        // A HOME_CSC selection wins over reference-only CSC under preserve.
        const uint32_t chosen=policy==EJODIN4_PRESERVE_DATA&&roles.count(EJODIN4_PACKAGE_HOME_CSC)?EJODIN4_PACKAGE_HOME_CSC:EJODIN4_PACKAGE_CSC;
        if(policy==EJODIN4_PRESERVE_DATA&&chosen==EJODIN4_PACKAGE_CSC&&!roles.count(EJODIN4_PACKAGE_USERDATA))return bad(error,"Preserve with CSC requires a selected USERDATA archive and its download list",EJODIN4_INVALID_ARGUMENT);
        if(!selected&&(!roles.count(1)||!roles.count(2)||!roles.count(3)||!roles.count(chosen)))return bad(error,"BL, AP, CP and the policy-selected CSC variant are required",EJODIN4_INVALID_ARGUMENT);
        std::unique_ptr<FirmwareSet> set(new FirmwareSet);set->data_policy=policy;
        std::vector<PlanImage> candidates;
        uint64_t validated_total=0;bcc::Array<uint8_t,65536> buffer;
        std::string selected_download_list; bool download_list_found=false;
        VerificationReporter reporter(progress);
        for(std::map<uint32_t,const EJODIN4_FirmwareFile*>::const_iterator pkg=roles.begin();pkg!=roles.end();++pkg){
            const std::pair<const uint32_t,const EJODIN4_FirmwareFile*>& package=*pkg;
            if(cancel.requested())return bad(error,"Firmware preflight cancelled",EJODIN4_CANCELLED);
            VerificationProgress work;work.phase=EJODIN4_PHASE_VERIFY_ARCHIVE;work.role=package.first;work.archive_index=uint32_t(set->archives.size());work.archive_count=count;work.name=archive_progress_name(*package.second);
            ArchiveProgressContext archive_context(&reporter,&work);
            const ArchiveScanCallback archive_progress(&ArchiveProgressContext::report,&archive_context);
            std::unique_ptr<HeldArchive> archive;EJODIN4_Status status=open_held_archive(*package.second,cancel,archive,error,archive_progress);if(status!=EJODIN4_OK)return status;
            const size_t archive_index=set->archives.size();set->archives.push_back(archive.get());archive.release();HeldArchive& held=*set->archives.back();
            for(size_t member=0;member<held.entries.size();++member){
                const EJODIN4_ArchiveEntry& entry=held.entries[member];PlanImage image;image.role=package.first;image.name=entry.name;image.source_offset=entry.data_offset;image.stored_bytes=entry.data_bytes;image.archive_index=archive_index;image.member_index=member;
                const bool is_pit=(entry.flags&EJODIN4_ARCHIVE_ENTRY_PIT)!=0;
                // Container structure and MD5 were verified when held. Reference
                // images are neither decoded nor scheduled; only its PIT is read.
                if(package.first==EJODIN4_PACKAGE_PIT_REFERENCE&&!is_pit){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_PIT_REFERENCE,image.name));continue;}
                std::unique_ptr<ImageReader> reader;status=held.open_member(member,reader,error);if(status!=EJODIN4_OK)return status;
                work.phase=EJODIN4_PHASE_VERIFY_IMAGE;work.name=image.name;work.completed_bytes=0;work.total_bytes=entry.data_bytes;reporter.report(work,true);

                Hash hash;if(!hash.start())return bad(error,"Cannot initialize SHA256",EJODIN4_INTERNAL_ERROR);
                SparseGeometry geometry;std::vector<uint8_t> pit;
                const bool is_download_list=!selected&&((package.first==chosen&&chosen==EJODIN4_PACKAGE_HOME_CSC)||package.first==EJODIN4_PACKAGE_USERDATA)&&fold(image.name)=="meta-data/download-list.txt";
                std::string member_download_list;
                for(;;){uint32_t actual=0;status=reader->read(buffer.data(),uint32_t(buffer.size()),actual,cancel,error);if(status!=EJODIN4_OK)return status;if(!actual)break;
                    if(actual>limit-validated_total)return bad(error,"Decoded firmware set exceeds its byte limit",EJODIN4_DECODE_LIMIT_EXCEEDED);
                    validated_total+=actual;image.decoded_bytes+=actual;if(!hash.add(buffer.data(),actual))return bad(error,"SHA256 update failed",EJODIN4_INTERNAL_ERROR);
                    if(is_download_list){if(actual>65536-member_download_list.size())return bad(error,"Selected download list exceeds 64 KiB");member_download_list.append(reinterpret_cast<const char*>(buffer.data()),actual);}
                    if(is_pit){if(actual>1048576-pit.size())return bad(error,"Package PIT exceeds 1 MiB",EJODIN4_INVALID_PIT);pit.insert(pit.end(),buffer.begin(),buffer.begin()+actual);}
                    else if(!metadata(image.name)){status=geometry.consume(buffer.data(),actual,error);if(status!=EJODIN4_OK)return status;}
                    report_member(reporter,work,*reader,entry.data_bytes,false);
                }
                if(!hash.finish(image))return bad(error,"SHA256 completion failed",EJODIN4_INTERNAL_ERROR);
                if(is_download_list){
                    if(download_list_found&&selected_download_list!=member_download_list)return bad(error,"Selected HOME_CSC and USERDATA download lists differ");
                    selected_download_list=member_download_list;download_list_found=true;
                }
                if(is_pit){
                    if(!set->package_pit_bytes.empty()&&set->package_pit_bytes!=pit)return bad(error,"Conflicting package PIT members");
                    report_member(reporter,work,*reader,entry.data_bytes,true);if(cancel.requested())return bad(error,"Firmware preflight cancelled",EJODIN4_CANCELLED);
                    set->package_pit_bytes=pit;set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_PIT,image.name));continue;
                }
                if(metadata(image.name)){report_member(reporter,work,*reader,entry.data_bytes,true);if(cancel.requested())return bad(error,"Firmware preflight cancelled",EJODIN4_CANCELLED);set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_METADATA,image.name));continue;}
                status=geometry.finish(image.decoded_bytes,image.expanded_bytes,error);if(status!=EJODIN4_OK)return status;
                report_member(reporter,work,*reader,entry.data_bytes,true);if(cancel.requested())return bad(error,"Firmware preflight cancelled",EJODIN4_CANCELLED);
                if(!selected&&package.first!=1&&package.first!=2&&package.first!=3&&package.first!=chosen&&package.first!=EJODIN4_PACKAGE_USERDATA){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_CSC_ALTERNATIVE,image.name));continue;}
                candidates.push_back(image);
            }
        }
        if(set->package_pit_bytes.empty())return bad(error,"A validated package PIT is required");
        set->package_pit.resize(512);uint32_t entries=0;char diagnostic[256]={0};
        EJODIN4_Status status=EJODIN4_ParsePit(&set->package_pit_bytes[0],uint32_t(set->package_pit_bytes.size()),&set->package_pit[0],512,&entries,diagnostic,sizeof(diagnostic));
        if(status!=EJODIN4_OK) return bad(error,"Package PIT failed strict validation",EJODIN4_INVALID_PIT);
        set->package_pit.resize(entries);
        std::map<std::string,const EJODIN4_PitEntry*> filenames;std::set<uint32_t> userdata_ids;
        for(size_t p=0;p<set->package_pit.size();++p){const EJODIN4_PitEntry& route=set->package_pit[p];const std::string name=fold(field(route.file_name,32));if(!name.empty()&&!filenames.insert(std::make_pair(name,&route)).second)return bad(error,"Package PIT has ambiguous image filenames");if(fold(field(route.partition_name,32))=="userdata"||basename(name)=="userdata.img")userdata_ids.insert(route.identifier);}
        std::set<std::string> download_names,represented_names;
        if(policy==EJODIN4_PRESERVE_DATA){
            if(!download_list_found||selected_download_list.empty())return bad(error,"Preserve policy requires a validated selected HOME_CSC or USERDATA download list");
            size_t begin=0;
            while(begin<selected_download_list.size()){
                const size_t end=selected_download_list.find('\n',begin);
                std::string line=selected_download_list.substr(begin,end==std::string::npos?std::string::npos:end-begin);
                if(!line.empty()&&line[line.size()-1]=='\r')line.resize(line.size()-1);
                if(line.empty()||line.size()>255||line=="."||line==".."||unsafe_basename(line)||!download_names.insert(fold(line)).second)return bad(error,"Selected download list has an unsafe or duplicate basename");
                if(end==std::string::npos) break;
                begin=end+1;
            }
        }
        std::map<uint32_t,size_t> destinations;
        for(size_t i=0;i<candidates.size();++i){PlanImage& image=candidates[i];
            const std::map<std::string,const EJODIN4_PitEntry*>::const_iterator found=filenames.find(basename(image.name));if(found==filenames.end())return bad(error,"A firmware image has no exact package PIT filename mapping");image.route=*found->second;
            const std::string route_name=field(image.route.partition_name,32);
            if(route_name.empty()||route_name.size()==32||MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,route_name.c_str(),int(route_name.size()),0,0)<=0||control_name(route_name))return bad(error,"Selected PIT partition name must be terminated UTF8");
            if(!image.decoded_bytes)return bad(error,"Empty firmware images cannot be transferred");
            const std::string partition_name=fold(route_name);
            image.large_partition=partition_name=="system"||partition_name=="userdata"||partition_name=="super";
            if(selected&&selected_protected(image)){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_USERDATA,image.name,image.route));continue;}
            if(policy==EJODIN4_PRESERVE_DATA&&(userdata(image)||userdata_ids.count(image.route.identifier))){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_USERDATA,image.name,image.route));continue;}
            if(policy==EJODIN4_PRESERVE_DATA){
                if(misc(image)){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_DOWNLOAD_LIST,image.name,image.route));continue;}
                const std::string image_name=basename(image.name);
                if(!download_names.count(image_name)){set->skips.push_back(PlanSkip(image.role,EJODIN4_SKIP_DOWNLOAD_LIST,image.name,image.route));continue;}
                represented_names.insert(image_name);
            }
            const std::map<uint32_t,size_t>::const_iterator existing=destinations.find(image.route.identifier);
            if(existing!=destinations.end()){
                const PlanImage& previous=set->images[existing->second];
                if(!same_route(previous.route,image.route))return bad(error,"Repeated partition destination differs in PIT route");
                // Odin sends validated repeats in slot/member order. Every repeat
                // retains its own payload, digest and length, including ordered
                // overwrites, and contributes to the advertised total.
            }
            if(image.decoded_bytes>UINT64_MAX-set->total_bytes)return bad(error,"Firmware transfer total overflows",EJODIN4_DECODE_LIMIT_EXCEEDED);
            destinations.insert(std::make_pair(image.route.identifier,set->images.size()));set->total_bytes+=image.decoded_bytes;set->images.push_back(image);
        }
        if(policy==EJODIN4_PRESERVE_DATA&&represented_names!=download_names)return bad(error,"Selected download list names a missing, excluded or unknown selected image");
        if(set->images.empty())return bad(error,"Firmware plan contains no images");
        status=set->verify_unchanged(error);if(status!=EJODIN4_OK)return status;output.reset(set.release());return EJODIN4_OK;
    }catch(...){return bad(error,"Firmware set preparation raised an exception",EJODIN4_INTERNAL_ERROR);}
}
}
