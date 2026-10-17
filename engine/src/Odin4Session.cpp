#include "Odin4Session.h"
#include <algorithm>
#include <cstring>
#include <utility>
#include <sstream>
namespace ejodin4 {
namespace {
std::string decimal(uint32_t value){std::ostringstream out;out<<value;return out.str();}
const uint32_t sequence_bytes = 30u * 1024u * 1024u;
uint32_t le32(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
void put32(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i) p[i]=uint8_t(v>>(i*8)); }
}
Session::Session(DownloadChannel& c,SessionProgress p,uint32_t timeout):channel_(c),progress_(p),packet_bytes_(131072),flash_timeout_(30000),timeout_override_(timeout),begun_(false),failed_(false),total_set_(false),metadata_baseline_set_(false),metadata_baseline_tail_(false),total_(0) {
    state_.transport=c.transport(); if(timeout) flash_timeout_=timeout;
}
void Session::phase(uint32_t p) { state_.phase=p; if(progress_) progress_(state_); }
EJODIN4_Status Session::fail(EJODIN4_Status s,const char* message,std::string& error) {
    failed_=true;
    if(error.empty()) error=message;
    phase(s==EJODIN4_CANCELLED?EJODIN4_PHASE_CANCELLED:EJODIN4_PHASE_FAILED);
    return s;
}
EJODIN4_Status Session::response(uint32_t expected,bool capability,uint32_t timeout,std::string& error,bool metadata) {
    uint8_t bytes[8]={0};
    EJODIN4_Status s=metadata?channel_.read_metadata_ack(bytes,timeout,state_.metadata_tail_present,state_.metadata_tail_word,error):channel_.read_exact(bytes,8,timeout,error);
    if(s!=EJODIN4_OK) return fail(s,"Read Odin acknowledgement failed",error);
    state_.last_response_id=le32(bytes); state_.last_ack=le32(bytes+4);
    if(state_.last_response_id!=expected || (!capability && (state_.last_ack&0x80000000u))) {
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
EJODIN4_Status Session::begin(std::vector<EJODIN4_PitEntry>& output,std::string& error) {
    if(begun_ || failed_ || !channel_.opened()) { error="Odin session cannot begin"; return EJODIN4_INVALID_ARGUMENT; }
    state_.transport=channel_.transport();
    phase(EJODIN4_PHASE_HANDSHAKE);
    EJODIN4_Status s=channel_.handshake(error);
    if(s!=EJODIN4_OK) return fail(s,"Odin handshake failed",error);
    phase(EJODIN4_PHASE_NEGOTIATING);
    uint32_t max_protocol=0x7fffffffu;
    s=command(0x64,0,&max_protocol,1,true,channel_.timeout(),error);
    if(s!=EJODIN4_OK) return s;
    state_.protocol_version=(state_.last_ack>>16)&0x7fffu;
    // Publish the unsigned capability word before later command ACKs replace it.
    phase(EJODIN4_PHASE_NEGOTIATING);
    if(state_.protocol_version>1) {
        packet_bytes_=1048576; if(!timeout_override_) flash_timeout_=120000;
        s=command(0x64,5,&packet_bytes_,1,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK) return s;
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
        s=channel_.read_exact(&pit[0]+offset,size,timeout_override_?timeout_override_:channel_.timeout(),error);
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
    s=EJODIN4_ParsePit(&pit[0],pit_size,&entries[0],512,&count,diagnostic,sizeof(diagnostic));
    if(s!=EJODIN4_OK) { error=std::string("Device PIT rejected: ")+diagnostic;return fail(s,"Device PIT parse failed",error); }
    entries.resize(count);
    // The pinned query and post-file counter reset have identical 64/1 wire
    // requests. Establish their 8/12-byte framing before any image can be sent.
    s=command(0x64,1,0,0,false,channel_.timeout(),error);
    if(s!=EJODIN4_OK)return s;
    pit_bytes_.swap(pit);
    output.swap(entries);begun_=true;
    phase(EJODIN4_PHASE_PIT_READY);
    return EJODIN4_OK;
}
EJODIN4_Status Session::notify_total(uint64_t total,std::string& error) {
    if(!begun_ || failed_ || total_set_ || !total) { error="Invalid Odin transfer total";return EJODIN4_INVALID_ARGUMENT; }
    const uint32_t words[]={uint32_t(total),uint32_t(total>>32)};
    EJODIN4_Status s=command(0x64,2,words,2,false,channel_.timeout(),error);
    if(s==EJODIN4_OK) { total_=total;total_set_=true; }
    return s;
}
EJODIN4_Status Session::send_image(const PlanImage& image,ImageReader& reader,std::string& error) {
    if(!begun_ || failed_ || !total_set_ || !image.decoded_bytes || image.decoded_bytes>total_-state_.acknowledged_bytes) {
        error="Invalid Odin image or transfer state";return EJODIN4_INVALID_ARGUMENT;
    }
    state_.image_acknowledged_bytes=0;
    uint64_t remaining=image.decoded_bytes;
    bool first=true;
    // A full sequence is decoded before XMITSTART. The device never waits for
    // archive reads or decoding between a sequence request and its data packets.
    std::vector<uint8_t> buffer(sequence_bytes);
    while(remaining) {
        const uint32_t original=uint32_t(std::min<uint64_t>(remaining,sequence_bytes));
        const uint32_t real=image.large_partition?((original+511u)/512u)*512u:original;
        const uint32_t aligned=((real+packet_bytes_-1u)/packet_bytes_)*packet_bytes_;
        std::fill(buffer.begin(),buffer.begin()+aligned,static_cast<uint8_t>(0));
        uint32_t filled=0;
        while(filled<original) {
            EJODIN4_Status s=channel_.interrupted(error);
            if(s!=EJODIN4_OK) return fail(s,"Image decode cancelled",error);
            uint32_t actual=0;
            s=reader.read(&buffer[0]+filled,original-filled,actual,channel_.cancellation(),error);
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
        if(first) {
            // File-begin may already change flash state on some Loke builds.
            state_.any_write=true;
            phase(EJODIN4_PHASE_IMAGE_BEGIN);
            EJODIN4_Status s=command(0x66,0,0,0,false,channel_.timeout(),error);
            if(s!=EJODIN4_OK) return s;
            first=false;
        }
        EJODIN4_Status s=command(0x66,2,&aligned,1,false,channel_.timeout(),error);
        if(s!=EJODIN4_OK) return s;
        for(uint32_t offset=0,index=0;offset<aligned;offset+=packet_bytes_,++index) {
            state_.any_write=true; // The following call may write partially before failure.
            s=channel_.write_exact(&buffer[0]+offset,packet_bytes_,flash_timeout_,error);
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
    const EJODIN4_Status reset=command(0x64,1,0,0,false,channel_.timeout(),error);
    if(reset!=EJODIN4_OK) return reset;
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
