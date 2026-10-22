#include "Odin4Session.h"
#include "Odin4LayoutProfile.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <sstream>
namespace ejodin4 {
namespace {
std::string decimal(uint32_t value){std::ostringstream out;out<<value;return out.str();}
const uint32_t sequence_bytes = 30u * 1024u * 1024u;
uint32_t stock_sequence_bytes(const EJODIN4_PitEntry& route) {
    if(route.binary_type!=0)return 0;
    switch(route.device_type) {
    case 1:case 2:case 8:return sequence_bytes;
    case 6:return 4u*1024u*1024u;
    case 7:return 1024u*1024u;
    default:return 0;
    }
}
uint32_t le32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(i*8)); }
}
bool stock_image_route_supported(const EJODIN4_PitEntry& route) { return stock_sequence_bytes(route)!=0; }
Session::Session(DownloadChannel& c,SessionProgress p,uint32_t timeout):channel_(c),progress_(p),packet_bytes_(131072),flash_timeout_(30000),timeout_override_(timeout),begun_(false),failed_(false),total_set_(false),identity_validated_(false),metadata_baseline_set_(false),metadata_baseline_tail_(false),total_(0) {
    state_.transport=c.transport(); if(timeout) flash_timeout_=timeout;
}
void Session::phase(uint32_t p) { state_.phase=p; if(progress_) progress_(state_); }
EJODIN4_Status Session::fail(EJODIN4_Status s,const char* message,std::string& error) {
    failed_=true;
    if(error.empty()) error=message;
    phase(s==EJODIN4_CANCELLED?EJODIN4_PHASE_CANCELLED:EJODIN4_PHASE_FAILED);
    return s;
}
EJODIN4_Status Session::response(uint32_t expected,bool capability,uint32_t timeout,std::string& error,bool metadata,bool allow_empty_id) {
    uint8_t bytes[8]={0};
    EJODIN4_Status s=metadata?channel_.read_metadata_ack(bytes,timeout,state_.metadata_tail_present,state_.metadata_tail_word,error):channel_.read_exact(bytes,8,timeout,error);
    if(s!=EJODIN4_OK) return fail(s,"Read Odin acknowledgement failed",error);
    state_.last_response_id=le32(bytes); state_.last_ack=le32(bytes+4);
    if((state_.last_response_id!=expected && !(allow_empty_id && state_.last_response_id==0)) || (!capability && (state_.last_ack&0x80000000u))) {
        error="Unexpected Odin acknowledgement id="+decimal(state_.last_response_id)+" ack="+decimal(state_.last_ack);
        return fail(EJODIN4_INVALID_RESPONSE,"Invalid Odin acknowledgement",error);
    }
    if(metadata) {
        if(metadata_baseline_set_ && metadata_baseline_tail_!=state_.metadata_tail_present) {
            error="Samsung reset response framing changed within one session";
            return fail(EJODIN4_INVALID_RESPONSE,"Changed metadata framing",error);
        }
        metadata_baseline_set_=true;metadata_baseline_tail_=state_.metadata_tail_present;
    }
    return EJODIN4_OK;
}
EJODIN4_Status Session::command(uint32_t cmd,uint32_t sub,const uint32_t* words,uint32_t count,bool capability,uint32_t timeout,std::string& error) {
    if(failed_ || count>9) { error="Odin session is failed or request is invalid"; return EJODIN4_INVALID_ARGUMENT; }
    uint8_t request[1024]={0};
    if(timeout_override_) timeout=timeout_override_;
    put32(request,cmd); put32(request+4,sub);
    for(uint32_t i=0;i<count;++i) put32(request+8+4*i,words[i]);
    EJODIN4_Status s=channel_.write_exact(request,sizeof(request),timeout,error);
    if(s!=EJODIN4_OK) return fail(s,"Write Odin request failed",error);
    return response(cmd,capability,timeout,error,cmd==0x64 && sub==1);
}
EJODIN4_Status Session::begin(std::vector<EJODIN4_PitEntry>& output,std::string& error,uint64_t planned_total,const FirmwareIdentity* identity,const std::vector<uint8_t>* repartition_pit) {
    if(begun_ || failed_ || !channel_.opened()) { error="Odin session cannot begin"; return EJODIN4_INVALID_ARGUMENT; }
    state_.transport=channel_.transport();
    if(repartition_pit) {
        if(!native_protocol()) {
            error="Repartition requires Samsung stock-driver or TCP transport";
            return fail(EJODIN4_DRIVER_UNSUPPORTED,"Unreviewed repartition transport",error);
        }
        if(!planned_total || !identity) { error="Repartition requires a preflighted firmware write context";return fail(EJODIN4_INVALID_ARGUMENT,"Missing repartition context",error); }
        std::vector<EJODIN4_PitEntry> candidate(512);uint32_t count=0;char text[256]={0};
        if(repartition_pit->empty() || repartition_pit->size()>1048576u ||
            EJODIN4_ParsePit(&(*repartition_pit)[0],uint32_t(repartition_pit->size()),&candidate[0],512,&count,text,sizeof(text))!=EJODIN4_OK) {
            error="Repartition candidate PIT is invalid";return fail(EJODIN4_INVALID_PIT,"Invalid candidate PIT",error);
        }
    }
    phase(EJODIN4_PHASE_HANDSHAKE);
    EJODIN4_Status s=channel_.handshake(error);
    if(s!=EJODIN4_OK) return fail(s,"Odin handshake failed",error);
    phase(EJODIN4_PHASE_NEGOTIATING);
    uint32_t max_protocol=native_protocol()?5u:0x7fffffffu;
    s=command(0x64,0,&max_protocol,1,true,channel_.timeout(),error);
    if(s!=EJODIN4_OK) return s;
    state_.negotiation_ack=state_.last_ack;
    const bool stock=native_protocol();
    state_.protocol_version=(state_.negotiation_ack>>16)&(stock?0xffffu:0x7fffu);
    phase(EJODIN4_PHASE_NEGOTIATING);
    if(state_.protocol_version<1 || state_.protocol_version>5 || (!stock&&state_.protocol_version>3)) {
        error="Unsupported Samsung Download protocol "+decimal(state_.protocol_version)+(stock?"; native serial/TCP dialects are 1..5":"; reviewed WinUSB dialects are 1..3");
        return fail(EJODIN4_DRIVER_UNSUPPORTED,"Unsupported Odin protocol",error);
    }
    if(state_.protocol_version>=4 && identity && (!identity->present || identity->model.empty())) {
        DeviceInfo unknown;const EJODIN4_Status status=validate_device_identity(unknown,*identity,state_.protocol_version,error);
        return fail(status,"Missing authenticated package identity",error);
    }
    if(state_.protocol_version>1) {
        packet_bytes_=1048576; if(!timeout_override_) flash_timeout_=120000;
        s=command(0x64,5,&packet_bytes_,1,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK) return s;
    }
    // Native stock-driver Odin announces the decoded transfer total before PIT.
    // This is session metadata only; image commands remain behind live layout validation.
    if(planned_total && native_protocol()) {
        s=announce_total(planned_total,error);
        if(s!=EJODIN4_OK) return s;
    }
    if(state_.protocol_version>=4){
        s=read_device_info(error);if(s!=EJODIN4_OK)return s;
        if(identity){
            s=validate_device_identity(device_info_,*identity,state_.protocol_version,error);
            if(s!=EJODIN4_OK)return fail(s,"Firmware DEVINFO identity rejected",error);
            identity_validated_=true;
        }else if(planned_total){
            error="Authenticated firmware identity metadata is required before newer-protocol writes";
            return fail(EJODIN4_DRIVER_UNSUPPORTED,"Missing firmware identity",error);
        }
    }
    if(repartition_pit) {
        // Intent callback precedes the cancellation boundary and does not prove a write.
        phase(EJODIN4_PHASE_PIT_UPLOAD_BEGIN);
        s=channel_.interrupted(error);
        if(s!=EJODIN4_OK)return fail(s,"Repartition cancelled before PIT upload",error);
        state_.any_write=true;state_.pit_write_started=true;
        // Native 0x435240: control, size, exact raw buffer, final size.
        // A failed first command can still have reached the device.
        s=command(0x65,0,0,0,false,flash_timeout_,error);
        if(s!=EJODIN4_OK)return s;
        const uint32_t size=uint32_t(repartition_pit->size());
        s=command(0x65,2,&size,1,false,flash_timeout_,error);
        if(s!=EJODIN4_OK)return s;
        s=channel_.write_exact(&(*repartition_pit)[0],size,timeout_override_?timeout_override_:flash_timeout_,error);
        if(s!=EJODIN4_OK)return fail(s,"Raw PIT upload failed",error);
        // Network Download implementations may use the generic data ACK after
        // raw PIT bytes. Control replies and serial PIT data still require 0x65.
        s=response(0x65,false,timeout_override_?timeout_override_:flash_timeout_,error,false,
            channel_.transport()==EJODIN4_DOWNLOAD_TCP);
        if(s!=EJODIN4_OK)return s;
        s=command(0x65,3,&size,1,false,flash_timeout_,error);
        if(s!=EJODIN4_OK)return s;
        state_.pit_uploaded=true;
        phase(EJODIN4_PHASE_PIT_UPLOAD_COMPLETE);
    }
    s=command(0x65,1,0,0,false,channel_.timeout(),error);
    if(s!=EJODIN4_OK) return s;
    const uint32_t pit_size=state_.last_ack;
    if(!pit_size || pit_size>1024u*1024u) { error="Device PIT size is outside 1..1 MiB"; return fail(EJODIN4_USB_IO_ERROR,"Invalid PIT size",error); }
    std::vector<uint8_t> pit(pit_size);
    uint8_t request[1024]={0};
    put32(request,0x65); put32(request+4,2);
    uint32_t offset=0,index=0;
    while(offset<pit_size) {
        put32(request+8,index++);
        s=channel_.write_exact(request,sizeof(request),timeout_override_?timeout_override_:channel_.timeout(),error);
        if(s!=EJODIN4_OK) return fail(s,"PIT block request failed",error);
        const uint32_t size=std::min<uint32_t>(500,pit_size-offset);
        s=channel_.read_exact((&pit[0])+offset,size,timeout_override_?timeout_override_:channel_.timeout(),error);
        if(s!=EJODIN4_OK) return fail(s,"Raw PIT block read failed",error);
        offset+=size;
    }
    uint32_t optional_id=0,optional_ack=0; bool optional=false;
    s=channel_.optional_pit_ack(optional_id,optional_ack,optional,error);
    if(s!=EJODIN4_OK) return fail(s,"Optional PIT frame failed",error);
    if(optional) {
        state_.last_response_id=optional_id;state_.last_ack=optional_ack;
        if(optional_id!=0x65 || (optional_ack&0x80000000u)) { error="Foreign or failed optional PIT acknowledgement"; return fail(EJODIN4_INVALID_RESPONSE,"Invalid optional PIT acknowledgement",error); }
    }
    if(channel_.transport()==EJODIN4_DOWNLOAD_WINUSB) {
        s=channel_.usb_boundary(true,error);
        if(s!=EJODIN4_OK) return fail(s,"PIT USB boundary failed",error);
    }
    s=command(0x65,3,0,0,false,channel_.timeout(),error);
    if(s!=EJODIN4_OK) return s;
    std::vector<EJODIN4_PitEntry> entries(512);
    uint32_t count=0;char diagnostic[512]={0};
    s=EJODIN4_ParsePit((&pit[0]),pit_size,(&entries[0]),512,&count,diagnostic,sizeof(diagnostic));
    if(s!=EJODIN4_OK) { error=std::string("Device PIT rejected: ")+diagnostic;return fail(s,"Device PIT parse failed",error); }
    entries.resize(count);
    if(repartition_pit) {
        if(!same_pit_layout(*repartition_pit,pit)) {
            error="Device PIT read-back differs from the uploaded candidate; no firmware images were sent";
            return fail(EJODIN4_PIT_MAPPING_ERROR,"Repartition read-back mismatch",error);
        }
        state_.pit_verified=true;
        phase(EJODIN4_PHASE_PIT_VERIFIED);
    }
    // The pinned query and post-file counter reset have identical 64/1 wire
    // requests. Establish their 8/12-byte framing before any image can be sent.
    if(channel_.transport()==EJODIN4_DOWNLOAD_WINUSB) {
        s=command(0x64,1,0,0,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK)return s;
    }
    pit_bytes_.swap(pit);
    output.swap(entries);begun_=true;
    phase(EJODIN4_PHASE_PIT_READY);
    return EJODIN4_OK;
}
EJODIN4_Status Session::read_device_info(std::string& error) {
    EJODIN4_Status status=command(0x69,0,0,0,false,channel_.timeout(),error);
    if(status!=EJODIN4_OK)return status;
    const uint32_t size=state_.last_ack;
    if(size<500||size>1000000||size%500){error="Samsung DEVINFO size is outside bounded 500-byte records";return fail(EJODIN4_INVALID_RESPONSE,"Invalid DEVINFO size",error);}
    std::vector<uint8_t> bytes(size);
    for(uint32_t offset=0,index=0;offset<size;offset+=500,++index){
        uint8_t request[1024]={0};put32(request,0x69);put32(request+4,1);put32(request+8,index);
        const uint32_t timeout=timeout_override_?timeout_override_:channel_.timeout();
        status=channel_.write_exact(request,sizeof(request),timeout,error);
        if(status!=EJODIN4_OK)return fail(status,"DEVINFO block request failed",error);
        status=channel_.read_exact((&bytes[0])+offset,500,timeout,error);
        if(status!=EJODIN4_OK)return fail(status,"DEVINFO block read failed",error);
    }
    status=command(0x69,2,0,0,false,channel_.timeout(),error);
    if(status!=EJODIN4_OK)return status;
    status=parse_device_info(bytes,state_.protocol_version,device_info_,error);
    if(status!=EJODIN4_OK)return fail(status,"Samsung DEVINFO rejected",error);
    return EJODIN4_OK;
}
EJODIN4_Status Session::notify_total(uint64_t total,std::string& error) {
    if(!begun_ || failed_ || total_set_ || !total) { error="Invalid Odin transfer total";return EJODIN4_INVALID_ARGUMENT; }
    return announce_total(total,error);
}
EJODIN4_Status Session::announce_total(uint64_t total,std::string& error) {
    // Actual Odin3 uses base UINT32_MAX, rather than a little-endian uint64.
    const bool stock=native_protocol();
    const uint32_t words[]={stock?uint32_t(total%UINT64_C(0xffffffff)):uint32_t(total),
        stock?uint32_t(total/UINT64_C(0xffffffff)):uint32_t(total>>32)};
    EJODIN4_Status s=command(0x64,2,words,2,false,channel_.timeout(),error);
    if(s==EJODIN4_OK) { total_=total;total_set_=true; }
    return s;
}
EJODIN4_Status Session::validate_image(const PlanImage& image,std::string& error) {
    if(!begun_ || failed_) {error="Odin session cannot validate image routes";return EJODIN4_INVALID_ARGUMENT;}
    if(state_.protocol_version>=4&&!identity_validated_){error="Firmware identity was not authenticated for this device";return fail(EJODIN4_DRIVER_UNSUPPORTED,"Missing DEVINFO identity validation",error);}
    if(native_protocol() && !stock_image_route_supported(image.route)) {
        error="Unsupported Samsung stock-driver image route: binary type "+decimal(image.route.binary_type)+
            ", device type "+decimal(image.route.device_type)+"; no reviewed raw transfer flow";
        return fail(EJODIN4_DRIVER_UNSUPPORTED,"Unsupported Odin route",error);
    }
    return EJODIN4_OK;
}
bool Session::compression_supported() const {
    return native_protocol() &&
        state_.protocol_version>=3 && (state_.negotiation_ack&0x8000u)!=0;
}
EJODIN4_Status Session::send_encoded_image(const PlanImage& image,EncodedImageReader& reader,std::string& error) {
    if(!compression_supported()) {
        error="Native encoded transfer was not negotiated";
        return fail(EJODIN4_DRIVER_UNSUPPORTED,"Unsupported encoded transfer",error);
    }
    const uint32_t sequence_limit=stock_sequence_bytes(image.route);
    const uint64_t records_limit=uint64_t(sequence_limit)+8u*(sequence_limit/65536u+1u);
    uint64_t remaining=image.decoded_bytes;
    while(remaining) {
        EJODIN4_Status s=channel_.interrupted(error);
        if(s!=EJODIN4_OK)return fail(s,"Encoded sequence cancelled",error);
        const uint32_t expected=uint32_t(std::min<uint64_t>(remaining,sequence_limit));
        uint32_t decoded=0;
        std::vector<uint8_t> records;
        s=reader.read_sequence(expected,records,decoded,channel_.cancellation(),error);
        if(s!=EJODIN4_OK)return fail(s,"Encoded sequence preparation failed",error);
        if(decoded!=expected || records.empty() || records.size()>records_limit) {
            error="Encoded sequence differs from the validated image bounds";
            return fail(EJODIN4_INVALID_COMPRESSED_DATA,"Invalid encoded sequence",error);
        }
        const bool last=remaining==decoded;
        if(last) {
            uint32_t extra_decoded=0;std::vector<uint8_t> extra;
            s=reader.read_sequence(sequence_limit,extra,extra_decoded,channel_.cancellation(),error);
            if(s!=EJODIN4_OK)return fail(s,"Encoded image EOF verification failed",error);
            if(extra_decoded || !extra.empty()) {
                error="Encoded image exceeds its validated decoded size";
                return fail(EJODIN4_INVALID_COMPRESSED_DATA,"Encoded image size mismatch",error);
            }
        }
        const uint32_t stored=uint32_t(records.size());
        const uint32_t aligned=((stored+packet_bytes_-1u)/packet_bytes_)*packet_bytes_;
        records.resize(aligned,0);
        s=channel_.interrupted(error);
        if(s!=EJODIN4_OK)return fail(s,"Encoded sequence cancelled before submission",error);
        // All local reading and verification precede the first modifying command.
        state_.any_write=true;
        phase(EJODIN4_PHASE_IMAGE_BEGIN);
        s=command(0x66,5,0,0,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK)return s;
        const uint32_t sizes[]={stored,decoded};
        s=command(0x66,6,sizes,2,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK)return s;
        for(uint32_t offset=0,index=0;offset<aligned;offset+=packet_bytes_,++index) {
            s=channel_.write_exact((&records[0])+offset,packet_bytes_,flash_timeout_,error);
            if(s!=EJODIN4_OK)return fail(s,"Encoded image packet write failed",error);
            s=response(0,false,flash_timeout_,error);
            if(s!=EJODIN4_OK)return s;
            if(state_.last_ack!=index) {
                error="Odin encoded packet acknowledgement index mismatch";
                return fail(EJODIN4_INVALID_RESPONSE,"Wrong encoded packet index",error);
            }
            // Packet ACKs prove stored records, not decoded partition bytes.
            phase(EJODIN4_PHASE_DATA_ACK);
        }
        const uint32_t words[]={0,decoded,image.route.binary_type,image.route.device_type,
            image.route.identifier,last?1u:0u,0,0};
        s=command(0x66,7,words,8,false,flash_timeout_,error);
        if(s!=EJODIN4_OK)return s;
        state_.acknowledged_bytes+=decoded;state_.image_acknowledged_bytes+=decoded;
        phase(EJODIN4_PHASE_DATA_ACK);
        remaining-=decoded;
    }
    ++state_.images_completed;
    phase(EJODIN4_PHASE_IMAGE_COMPLETE);
    return EJODIN4_OK;
}
EJODIN4_Status Session::send_image(const PlanImage& image,ImageReader& reader,std::string& error,EncodedImageReader* encoded) {
    if(!begun_ || failed_ || !total_set_ || !image.decoded_bytes || image.decoded_bytes>total_-state_.acknowledged_bytes) {
        error="Invalid Odin image or transfer state";return EJODIN4_INVALID_ARGUMENT;
    }
    const EJODIN4_Status validated=validate_image(image,error);
    if(validated!=EJODIN4_OK)return validated;
    state_.image_acknowledged_bytes=0;
    if(encoded)return send_encoded_image(image,*encoded,error);
    uint64_t remaining=image.decoded_bytes;
    bool first=true;
    // The native raw dispatcher uses the same framing for versions 1..5;
    // legacy negotiation changes packet size, not sequence begin/completion.
    const bool native_raw=native_protocol();
    const uint32_t sequence_limit=native_raw?stock_sequence_bytes(image.route):sequence_bytes;
    // A full sequence is decoded before XMITSTART. The device never waits for
    // archive reads or decoding between a sequence request and its data packets.
    std::vector<uint8_t> buffer(sequence_limit);
    while(remaining) {
        const uint32_t original=uint32_t(std::min<uint64_t>(remaining,sequence_limit));
        const uint32_t real=!native_raw && image.large_partition?((original+511u)/512u)*512u:original;
        const uint32_t aligned=((real+packet_bytes_-1u)/packet_bytes_)*packet_bytes_;
        std::fill(buffer.begin(),buffer.begin()+aligned,static_cast<uint8_t>(0));
        uint32_t filled=0;
        while(filled<original) {
            EJODIN4_Status s=channel_.interrupted(error);
            if(s!=EJODIN4_OK) return fail(s,"Image decode cancelled",error);
            uint32_t actual=0;
            s=reader.read((&buffer[0])+filled,original-filled,actual,channel_.cancellation(),error);
            if(s!=EJODIN4_OK) return fail(s,"Read image stream failed",error);
            if(!actual || actual>original-filled) { error="Image stream ended early or returned excess bytes";return fail(EJODIN4_USB_IO_ERROR,"Image size mismatch",error); }
            filled+=actual;
        }
        if(remaining==original) {
            uint8_t extra=0;uint32_t actual=0;
            EJODIN4_Status s=reader.read(&extra,1,actual,channel_.cancellation(),error);
            if(s!=EJODIN4_OK) return fail(s,"Image EOF verification failed",error);
            if(actual) { error="Image stream exceeds validated decoded size";return fail(EJODIN4_USB_IO_ERROR,"Image size mismatch",error); }
        }
        if(first || native_raw) {
            // File-begin may already change flash state on some Loke builds.
            state_.any_write=true;
            phase(EJODIN4_PHASE_IMAGE_BEGIN);
            EJODIN4_Status s=command(0x66,0,0,0,false,channel_.timeout(),error);
            if(s!=EJODIN4_OK) return s;
            first=false;
        }
        // The start command is 128-KiB aligned, while actual writes remain full
        // negotiated packets, including zero padding in the last packet.
        const uint32_t start_bytes=native_raw?((original+131071u)/131072u)*131072u:aligned;
        EJODIN4_Status s=command(0x66,2,&start_bytes,1,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK) return s;
        for(uint32_t offset=0,index=0;offset<aligned;offset+=packet_bytes_,++index) {
            state_.any_write=true; // The following call may write partially before failure.
            s=channel_.write_exact((&buffer[0])+offset,packet_bytes_,flash_timeout_,error);
            if(s!=EJODIN4_OK) return fail(s,"Image packet write failed",error);
            s=response(0,false,flash_timeout_,error);
            if(s!=EJODIN4_OK) return s;
            if(state_.last_ack!=index) { error="Odin packet acknowledgement index mismatch";return fail(EJODIN4_INVALID_RESPONSE,"Wrong packet index",error); }
            const uint64_t genuine=offset<original?std::min<uint32_t>(packet_bytes_,original-offset):0;
            state_.acknowledged_bytes+=genuine;state_.image_acknowledged_bytes+=genuine;
            phase(EJODIN4_PHASE_DATA_ACK);
        }
        s=channel_.usb_boundary(false,error);
        if(s!=EJODIN4_OK) return fail(s,"Sequence USB boundary failed",error);
        const bool last=remaining==original;
        const uint32_t words[]={image.route.binary_type==1?1u:0u,real,image.route.binary_type,image.route.device_type,
            image.route.binary_type==1?0u:image.route.identifier,last?1u:0u,0,0};
        s=command(0x66,3,words,8,false,flash_timeout_,error);
        if(s!=EJODIN4_OK) return s;
        if(channel_.transport()==EJODIN4_DOWNLOAD_WINUSB) {
            s=channel_.usb_boundary(true,error);
            if(s!=EJODIN4_OK) return fail(s,"Sequence USB completion boundary failed",error);
        }
        remaining-=original;
    }
    ++state_.images_completed;
    // Pinned odin4 resets its flash counter after each successfully completed
    // file. A failed reset cannot undo that file, but forbids further commands.
    if(channel_.transport()==EJODIN4_DOWNLOAD_WINUSB) {
        const EJODIN4_Status reset=command(0x64,1,0,0,false,channel_.timeout(),error);
        if(reset!=EJODIN4_OK) return reset;
    }
    phase(EJODIN4_PHASE_IMAGE_COMPLETE);
    return EJODIN4_OK;
}
EJODIN4_Status Session::finish(bool reboot,std::string& error) {
    if(!begun_ || failed_ || state_.finalized || (total_set_ && state_.acknowledged_bytes!=total_)) {
        error="Odin session cannot finalize incomplete or failed images";return EJODIN4_INVALID_ARGUMENT;
    }
    phase(EJODIN4_PHASE_FINALIZING);
    EJODIN4_Status s=command(0x67,0,0,0,false,channel_.timeout(),error);
    if(s!=EJODIN4_OK) return s;
    state_.finalized=true;
    if(reboot) {
        phase(EJODIN4_PHASE_REBOOT);
        s=command(0x67,1,0,0,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK) return s;
        state_.reboot_sent=true;
    }
    s=close(error);
    if(s!=EJODIN4_OK) return s;
    phase(EJODIN4_PHASE_COMPLETE);
    return EJODIN4_OK;
}
EJODIN4_Status Session::close(std::string& error) {
    EJODIN4_Status s=channel_.finish(error);
    if(s!=EJODIN4_OK) return fail(s,"Download connection cleanup failed",error);
    return EJODIN4_OK;
}
}
