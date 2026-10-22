#include "Odin4Session.h"
#include "Odin4FlashOperation.h"
#include "lz4frame.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <windows.h>
using namespace ejodin4;
namespace {
unsigned checks=0;
void check(bool condition,const char* message) { ++checks;if(!condition) throw std::runtime_error(message); }
uint32_t get(const uint8_t* p) { return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24; }
void put(uint8_t* p,uint32_t v) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(v>>(8*i)); }
struct Device {
    uint64_t clock,total,image_bytes,data_written;
    uint32_t protocol,packet,expected,packet_index,packets;
    uint32_t commands,images,sequences,finishes,closes,zero_calls;
    uint32_t wrong_command,fail_command,wrong_index;
    bool fail_reset,stock;
    bool encoded_packets,cancel_encoded_packet,negative_data_ack;
    uint32_t encoded_bytes,encoded_written,fail_sub;
    std::vector<uint32_t> encoded_completions;
    uint32_t data_ack_bytes;std::vector<uint32_t> data_packet_sizes;
    uint32_t handshake_write_fail_after,handshake_read_fail_after,handshake_read_bytes;
    EJODIN4_Status handshake_failure;
    uint32_t optional_mode,read_zeroes,zero_advance;
    uint32_t metadata_tail_bytes;
    bool change_metadata_framing;
    bool cancelled,fragment,fail_close,fail_write;
    bool handshake_bad,begin_capability,zero_cancel;
    bool pit_short,completion_fail;
    uint32_t pit_size_override;
    uint32_t data_ack_delay_ms;
    uint64_t data_ack_ready_at,data_ack_budget_ms,data_write_budget_ms;
    bool awaiting_data_ack;
    std::vector<uint64_t> control_budgets;
    std::vector<uint8_t> pit,write_buffer,devinfo;
    uint32_t devinfo_size_override;bool devinfo_short;
    std::deque<uint8_t> incoming;
    std::vector<std::vector<uint32_t> > transcript;
    bool pit_upload_mode,pit_raw_expected,pit_keep_old;
    uint32_t pit_starts,pit_uploads,pit_upload_bytes,pit_fault_stage,pit_fault_mode,pit_raw_limit,pit_raw_calls,pit_raw_fragment,pit_readback_size,pit_raw_ack_id;
    EJODIN4_Status pit_raw_failure;
    std::vector<uint8_t> uploaded_pit;
    Device() : devinfo_size_override(0),devinfo_short(false),encoded_packets(false),cancel_encoded_packet(false),negative_data_ack(false),encoded_bytes(0),encoded_written(0),fail_sub(UINT32_MAX),clock(0),total(0),image_bytes(0),data_written(0),protocol(2),packet(1048576),expected(4),packet_index(0),packets(0),commands(0),images(0),sequences(0),finishes(0),closes(0),zero_calls(0),wrong_command(0),fail_command(0),wrong_index(UINT32_MAX),fail_reset(false),optional_mode(0),read_zeroes(0),zero_advance(0),metadata_tail_bytes(0),change_metadata_framing(false),cancelled(false),fragment(true),fail_close(false),fail_write(false),handshake_bad(false),begin_capability(true),zero_cancel(false),pit_short(false),completion_fail(false),pit_size_override(0),data_ack_delay_ms(0),data_ack_ready_at(0),data_ack_budget_ms(0),data_write_budget_ms(0),awaiting_data_ack(false),stock(true),data_ack_bytes(8),handshake_write_fail_after(UINT32_MAX),handshake_read_fail_after(UINT32_MAX),handshake_read_bytes(0),handshake_failure(EJODIN4_TIMEOUT),pit(28+132,0) {
        pit_upload_mode=pit_raw_expected=pit_keep_old=false;pit_starts=pit_uploads=pit_upload_bytes=pit_raw_calls=0;
        pit_fault_stage=UINT32_MAX;pit_fault_mode=0;pit_raw_limit=UINT32_MAX;pit_raw_fragment=17;pit_raw_failure=EJODIN4_USB_IO_ERROR;pit_readback_size=0;pit_raw_ack_id=0x65;
        put((&pit[0]),0x12349876);put((&pit[0])+4,1);
        put((&pit[0])+28+8,1); // identifier
        put((&pit[0])+28+24,0x100000);
        std::memcpy((&pit[0])+28+36,"BOOT",4);
        std::memcpy((&pit[0])+28+68,"boot.img",8);
        devinfo.assign(500,0);put(&devinfo[0],0x12345678);put(&devinfo[0]+4,2);
        put(&devinfo[0]+8,0);put(&devinfo[0]+12,48);std::memcpy(&devinfo[0]+56,"SM-TEST",7);
        put(&devinfo[0]+20,3);put(&devinfo[0]+24,80);std::memcpy(&devinfo[0]+88,"PLS",3);
    }
    void queue(const uint8_t* p,size_t n) { incoming.insert(incoming.end(),p,p+n); }
    void ack(uint32_t id,uint32_t value=0) {
        uint8_t b[8]={0};put(b,id);put(b+4,value);queue(b,8);
    }
    void pit_ack(uint32_t stage,uint32_t value=0) {
        const bool fault=pit_fault_stage==stage;
        ack(fault&&pit_fault_mode==2?0x66:stage==2?pit_raw_ack_id:0x65,fault&&pit_fault_mode==1?UINT32_MAX:fault&&pit_fault_mode==5?7:value);
        if(fault&&pit_fault_mode==3)for(unsigned i=0;i<5;++i)incoming.pop_back();
        if(fault&&pit_fault_mode==4)cancelled=true;
    }
    void received() {
        if(expected==4) {
            check(!std::memcmp((&write_buffer[0]),"ODIN",4),"four byte handshake");
            queue(reinterpret_cast<const uint8_t*>(handshake_bad?"NOPE":"LOKE"),4);
            expected=1024;
        } else if(pit_raw_expected) {
            uploaded_pit=write_buffer;pit_raw_expected=false;expected=1024;
            pit_ack(2);
        } else if(packets) {
            data_packet_sizes.push_back(uint32_t(write_buffer.size()));
            if(encoded_packets){
                uint32_t genuine=std::min(encoded_bytes-encoded_written,packet);
                for(uint32_t i=0;i<genuine;++i)check(write_buffer[i]==0x6b,"encoded original record byte");
                for(size_t i=genuine;i<write_buffer.size();++i)check(write_buffer[i]==0,"encoded zero packet padding");encoded_written+=genuine;
            }else{
                check(write_buffer.front()==0x5a,"literal image byte");
                const uint64_t remaining=image_bytes-data_written;const uint32_t genuine=uint32_t(std::min<uint64_t>(remaining,packet));
                if(genuine<packet)check(write_buffer[genuine]==0&&write_buffer.back()==0,"last packet zero padding");data_written+=genuine;
            }
            ack(0,negative_data_ack?UINT32_MAX:wrong_index==packet_index?packet_index+1:packet_index);
            for(uint32_t i=data_ack_bytes;i<8;++i)incoming.pop_back();
            if(encoded_packets&&cancel_encoded_packet)cancelled=true;
            awaiting_data_ack=true;data_ack_ready_at=clock+data_ack_delay_ms;
            ++packet_index;--packets;expected=packets?packet:1024;
        } else {
            check(write_buffer.size()==1024,"control size");
            const uint32_t cmd=get((&write_buffer[0])),sub=get((&write_buffer[0])+4);
            std::vector<uint32_t> request;
            for(unsigned i=0;i<10;++i) request.push_back(get((&write_buffer[0])+4*i));
            transcript.push_back(request);++commands;
            for(size_t i=40;i<write_buffer.size();++i) check(write_buffer[i]==0,"control zero tail");
            if(cmd==0x64 && sub==0) check(request[2]==(stock?5u:0x7fffffffu),"transport-specific maximum protocol negotiation payload");
            if(cmd==0x64 && sub==5) check(request[2]==1048576,"modern packet size negotiation");
            if(cmd==0x66 && sub==2) check(request[2]>0 && request[2]<=30u*1024u*1024u && request[2]%(stock?131072u:packet)==0,"bounded aligned sequence size");
            if(cmd==0x69 && sub==1){
                uint32_t offset=request[2]*500;check(offset+500<=devinfo.size(),"bounded DEVINFO block index");queue(&devinfo[0]+offset,devinfo_short?499:500);
            } else if(cmd==0x65 && sub==0) {
                check(!pit_upload_mode,"PIT upload does not restart");++pit_starts;pit_upload_mode=true;pit_ack(0);
            } else if(cmd==0x65 && sub==2 && pit_upload_mode) {
                check(request[2]>0,"PIT upload declares nonzero exact length");pit_upload_bytes=request[2];
                pit_raw_expected=true;expected=pit_upload_bytes;pit_ack(1);
            } else if(cmd==0x65 && sub==3 && pit_upload_mode) {
                check(request[2]==pit_upload_bytes&&uploaded_pit.size()==pit_upload_bytes,"PIT upload final declares original raw length");
                ++pit_uploads;if(!pit_keep_old){pit=uploaded_pit;if(pit_readback_size)pit.resize(pit_readback_size,0x9d);}pit_upload_mode=false;pit_ack(3);
            } else if(cmd==0x65 && sub==2) {
                const uint32_t offset=get((&write_buffer[0])+8)*500;
                check(offset<pit.size(),"PIT block index");
                const size_t n=std::min<size_t>(500,pit.size()-offset);
                queue((&pit[0])+offset,pit_short?n-1:n);
                if(pit_starts&&pit_fault_stage==5){
                    if(pit_fault_mode==3&&!incoming.empty())incoming.pop_back();
                    else if(pit_fault_mode==1&&!incoming.empty())incoming.front()^=0xff;
                    else if(pit_fault_mode==4)cancelled=true;
                }
                if(offset+n==pit.size()) {
                    if(optional_mode==1) ack(0x65);
                    else if(optional_mode==2) { uint8_t partial[3]={0x65,0,0};queue(partial,3); }
                    else if(optional_mode==3) ack(0x66);
                    else if(optional_mode==4) ack(0x65,0xfffffffeu);
                }
            } else if(cmd==0x65 && sub==1 && pit_starts) {
                pit_ack(4,pit_size_override?pit_size_override:uint32_t(pit.size()));
            } else if(cmd==0x65 && sub==3 && pit_starts) {
                pit_ack(6);
            } else {
                uint32_t value=0;
                if(cmd==0x64 && sub==0) value=(protocol<<16)|(begin_capability?(stock?0x8000u:0x80000000u):0);
                if(cmd==0x69 && sub==0)value=devinfo_size_override?devinfo_size_override:uint32_t(devinfo.size());
                if(cmd==0x65 && sub==1) value=pit_size_override?pit_size_override:uint32_t(pit.size());
                if(cmd==0x64 && sub==2) total=stock?uint64_t(get((&write_buffer[0])+8))+uint64_t(get((&write_buffer[0])+12))*UINT64_C(0xffffffff):uint64_t(get((&write_buffer[0])+8))|(uint64_t(get((&write_buffer[0])+12))<<32);
                if(cmd==0x66 && (sub==0||sub==5)){++images;encoded_packets=sub==5;}
                if(cmd==0x66 && sub==6){++sequences;encoded_bytes=request[2];encoded_written=0;packets=(encoded_bytes+packet-1)/packet;packet_index=0;expected=packet;}
                if(cmd==0x66 && sub==7)encoded_completions.push_back(request[3]);
                if(cmd==0x66 && sub==2) { ++sequences;packets=(get((&write_buffer[0])+8)+packet-1)/packet;packet_index=0;expected=packet; }
                if(cmd==0x66 && (sub==3||sub==7) && completion_fail) value=0xfffffffeu;
                if(cmd==0x64 && sub==1 && fail_reset) value=0xfffffffeu;
                if(cmd==0x67 && sub==0) ++finishes;
                if(cmd==fail_command && (fail_sub==UINT32_MAX||fail_sub==sub)) value=0xfffffffeu;
                ack(cmd==wrong_command?cmd+1:cmd,value);
                if(cmd==0x64 && sub==1) {
                    uint8_t tail[4]={0x78,0x56,0x34,0x12};
                    const uint32_t size=change_metadata_framing && images?4u:metadata_tail_bytes;
                    queue(tail,size);
                }
            }
        }
        write_buffer.clear();
    }
    EJODIN4_Status transfer(bool read,uint8_t* p,uint32_t n,uint32_t& actual,uint64_t until,std::string& error) {
        actual=0;
        if(cancelled) return EJODIN4_CANCELLED;
        if(!n) {++zero_calls;return EJODIN4_OK;}
        const bool handshake_read=read && expected==1024 && commands==0;
        if((!read && expected==4 && write_buffer.size()>=handshake_write_fail_after) || (handshake_read && handshake_read_bytes>=handshake_read_fail_after)){clock=until;error="injected handshake terminal Windows error 995";return handshake_failure;}
        if(read) {
            if(awaiting_data_ack&&data_ack_ready_at>clock){
                data_ack_budget_ms=until-clock;
                if(data_ack_ready_at>=until){clock=until;error="delayed fake data ACK exceeded deadline";return EJODIN4_TIMEOUT;}
                clock=data_ack_ready_at;
            }
            if(read_zeroes) {--read_zeroes;clock+=zero_advance;if(zero_cancel)cancelled=true;return EJODIN4_OK;}
            if(incoming.empty()) {clock=until;error="fake timeout";return EJODIN4_TIMEOUT;}
            actual=uint32_t(std::min<size_t>(n,incoming.size()));
            if(fragment) actual=std::min<uint32_t>(actual,3);
            if(handshake_read){actual=std::min(actual,handshake_read_fail_after-handshake_read_bytes);handshake_read_bytes+=actual;}
            for(uint32_t i=0;i<actual;++i){p[i]=incoming.front();incoming.pop_front();}
            if(incoming.empty())awaiting_data_ack=false;
        } else {
            if(pit_raw_expected){
                ++pit_raw_calls;
                if(write_buffer.size()>=pit_raw_limit){error="injected raw PIT terminal transfer";return pit_raw_failure;}
                actual=std::min<uint32_t>(n,expected-uint32_t(write_buffer.size()));
                actual=std::min(actual,pit_raw_fragment);actual=std::min(actual,pit_raw_limit-uint32_t(write_buffer.size()));
                write_buffer.insert(write_buffer.end(),p,p+actual);if(write_buffer.size()==expected)received();return EJODIN4_OK;
            }
            if(write_buffer.empty()){
                if(expected==packet)data_write_budget_ms=until-clock;
                else control_budgets.push_back(until-clock);
            }
            if(fail_write && expected==packet) {error="injected uncertain write";return EJODIN4_USB_IO_ERROR;}
            actual=std::min<uint32_t>(n,expected-uint32_t(write_buffer.size()));
            if(fragment && expected<=1024) actual=std::min<uint32_t>(actual,17);
            if(expected==4)actual=std::min<uint32_t>(actual,handshake_write_fail_after-uint32_t(write_buffer.size()));
            write_buffer.insert(write_buffer.end(),p,p+actual);
            if(write_buffer.size()==expected) received();
        }
        return EJODIN4_OK;
    }
};
class Serial : public SerialConnection {
    Device& d_;
public:
    explicit Serial(Device& d):d_(d){}
    EJODIN4_Status transfer(bool r,uint8_t* p,uint32_t n,uint32_t& a,uint64_t t,const Cancellation&,std::string& e) {return d_.transfer(r,p,n,a,t,e);}
    EJODIN4_Status read_optional_frame(uint8_t* p,uint32_t size,bool& present,uint64_t deadline,const Cancellation&,std::string& e) {
        present=false;
        if(d_.cancelled)return EJODIN4_CANCELLED;
        if(d_.incoming.empty()){d_.clock=deadline;return EJODIN4_OK;}
        if(d_.incoming.size()!=size){e="partial or excess optional frame remains queued";return EJODIN4_USB_IO_ERROR;}
        for(unsigned i=0;i<size;++i){p[i]=d_.incoming.front();d_.incoming.pop_front();}
        present=true;return EJODIN4_OK;
    }
    EJODIN4_Status finish(std::string& e) {++d_.closes;if(d_.fail_close){e="restore failed";return EJODIN4_USB_IO_ERROR;}return EJODIN4_OK;}
};
class Usb : public Connection {
    Device& d_;
public:
    explicit Usb(Device& d):d_(d){}
    EJODIN4_Status interfaces(std::vector<Interface>& v,std::string&) {Interface i; i.number=0;i.device_class=10;Pipe in={0x81,512,true},out={2,512,true};i.pipes.push_back(in);i.pipes.push_back(out);v.clear();v.push_back(i);return EJODIN4_OK;}
    EJODIN4_Status transfer(size_t,uint8_t p,uint8_t* b,uint32_t n,uint32_t& a,uint64_t t,const Cancellation&,std::string& e) {
        const bool fragment=d_.fragment;
        if((p&0x80) && n==512)d_.fragment=false; // One USB metadata response packet.
        const EJODIN4_Status s=d_.transfer((p&0x80)!=0,b,n,a,t,e);d_.fragment=fragment;return s;
    }
};
class FakePlatform : public Platform {
public:
    Device d;
    EJODIN4_UsbDevice identity;
    bool duplicate,missing;unsigned enumerations,tcp_opens;
    uint64_t open_budget_ms;
    explicit FakePlatform(bool usb=false):duplicate(false),missing(false),enumerations(0),tcp_opens(0),open_budget_ms(0) {d.stock=!usb;std::memset(&identity,0,sizeof(identity));identity.vendor_id=0x04e8;identity.product_id=0x685d;identity.flags=usb?1:2;std::memcpy(identity.device_path,"bound-interface",16);}
    uint64_t now() const {return d.clock;}
    EJODIN4_Status enumerate(std::vector<EJODIN4_UsbDevice>& v,const Cancellation&,std::string&) {++enumerations;if(!missing)v.push_back(identity);if(duplicate)v.push_back(identity);return EJODIN4_OK;}
    EJODIN4_Status open(const EJODIN4_UsbDevice&,std::unique_ptr<Connection>& p,uint64_t,const Cancellation&,std::string&) {p.reset(new Usb(d));return EJODIN4_OK;}
    EJODIN4_Status open_tcp(const char*,std::unique_ptr<SerialConnection>& p,uint64_t deadline,const Cancellation&,std::string&) {++tcp_opens;open_budget_ms=deadline-d.clock;p.reset(new Serial(d));return EJODIN4_OK;}
    EJODIN4_Status open_serial(const EJODIN4_UsbDevice&,std::unique_ptr<SerialConnection>& p,uint64_t deadline,const Cancellation&,std::string&) {open_budget_ms=deadline-d.clock;p.reset(new Serial(d));return EJODIN4_OK;}
};
int32_t EJODIN4_CALL cancellation(void* p) {return static_cast<Device*>(p)->cancelled?1:0;}
class Reader : public ImageReader {
public:
    uint64_t remaining;
    bool too_long,fail;
    explicit Reader(uint64_t n):remaining(n),too_long(false),fail(false){}
    EJODIN4_Status read(uint8_t* p,uint32_t n,uint32_t& a,const Cancellation&,std::string& e) {
        a=0;if(fail){e="decode failed";return EJODIN4_INVALID_COMPRESSED_DATA;}
        a=uint32_t(std::min<uint64_t>(remaining,n));
        if(!a && too_long)a=1;
        std::memset(p,0x5a,a);remaining-=std::min<uint64_t>(remaining,a);return EJODIN4_OK;
    }
};
Cancellation make_cancel(Device* d){Cancellation c={cancellation,d};return c;}
class EncodedReader : public EncodedImageReader {
public:
    uint64_t remaining;
    uint32_t calls,fail_call,stored;
    bool short_decoded,empty_records,oversized,extra;
    Device* cancel_device;
    std::vector<uint32_t> record_sizes;
    explicit EncodedReader(uint64_t bytes):remaining(bytes),calls(0),fail_call(0),stored(73),short_decoded(false),empty_records(false),oversized(false),extra(false),cancel_device(0){}
    EJODIN4_Status read_sequence(uint32_t limit,std::vector<uint8_t>& records,uint32_t& decoded,
        const Cancellation&,std::string& error) {
        ++calls;records.clear();decoded=0;
        if(fail_call==calls){error="injected encoded reader error";return EJODIN4_CHECKSUM_MISMATCH;}
        if(cancel_device){cancel_device->cancelled=true;error="encoded preparation cancelled";return EJODIN4_CANCELLED;}
        decoded=uint32_t(std::min<uint64_t>(remaining,limit));
        if(!decoded){if(extra){decoded=1;records.assign(1,0x6b);}return EJODIN4_OK;}
        remaining-=decoded;if(short_decoded)--decoded;
        uint32_t bytes=stored;if(calls<=record_sizes.size())bytes=record_sizes[calls-1];
        if(oversized)bytes=31457280u+8u*(31457280u/65536u+1u)+1u;
        if(!empty_records)records.assign(bytes,0x6b);
        return EJODIN4_OK;
    }
};
struct Fixture {
    FakePlatform p;
    DownloadChannel c;
    Session s;
    std::string error;
    std::vector<EJODIN4_PitEntry> pit;
    bool tcp;
    explicit Fixture(bool usb=false,uint32_t timeout_override=0,bool network=false):p(usb),c(p,make_cancel(&p.d)),s(c,SessionProgress(),timeout_override),tcp(network){}
    EJODIN4_Status begin(uint64_t total=0,const FirmwareIdentity* identity=0,const std::vector<uint8_t>* target=0){const char* path=tcp?"tcp-listen://127.0.0.1:13579":"bound-interface";EJODIN4_Status r=c.open(path,uint32_t(std::strlen(path)+1),30000,error);return r==EJODIN4_OK?s.begin(pit,error,total,identity,target):r;}
    PlanImage image(uint64_t n,bool large=false) {PlanImage i;i.decoded_bytes=n;i.route.identifier=1;i.route.device_type=2;i.large_partition=large;p.d.image_bytes=n;return i;}
};

void tcp_session_tests(){
    FirmwareIdentity identity;identity.present=true;identity.model="SM-TEST_BUILD";
    for(unsigned protocol=1;protocol<=5;++protocol){
        Fixture f(false,0,true);f.p.d.protocol=protocol;f.p.d.packet=protocol==1?131072:1048576;
        check(f.begin(513,&identity)==EJODIN4_OK,"TCP negotiates each native protocol and reads PIT");
        check(f.p.enumerations==0&&f.p.tcp_opens==1&&f.s.state().transport==EJODIN4_DOWNLOAD_TCP,"TCP bypasses USB enumeration and preserves network identity");
        check(f.p.d.total==513&&f.p.d.incoming.empty(),"TCP announces total before PIT without optional frames");
        PlanImage image=f.image(513);Reader raw(513);check(f.s.send_image(image,raw,f.error)==EJODIN4_OK,"TCP raw image acknowledged");
        unsigned starts=0,completes=0;
        for(size_t i=0;i<f.p.d.transcript.size();++i){const std::vector<uint32_t>& row=f.p.d.transcript[i];
            check(!(row[0]==0x64&&row[1]==1),"TCP does not send WinUSB counter resets");
            if(row[0]==0x66&&row[1]==2){++starts;check(row[2]==131072,"TCP start size is 128 KiB aligned");}
            if(row[0]==0x66&&row[1]==3){++completes;check(row[3]==513&&row[5]==2&&row[6]==1,"TCP completion carries exact decoded length and PIT route");}
        }
        check(starts==1&&completes==1&&f.s.state().acknowledged_bytes==513,"TCP credits only acknowledged image bytes");
        check(f.s.finish(true,f.error)==EJODIN4_OK&&f.s.state().finalized&&f.s.state().reboot_sent,"TCP finalization and reboot require replies");
        check(f.p.d.zero_calls==0&&f.p.d.closes==1,"TCP has no USB zero packets and closes once");
    }
    {Fixture f(false,0,true);f.p.d.protocol=3;check(f.begin(513)==EJODIN4_OK&&f.s.compression_supported(),"TCP negotiated compressed download");PlanImage image=f.image(513);Reader raw(513);raw.fail=true;EncodedReader encoded(513);check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_OK&&f.s.finish(false,f.error)==EJODIN4_OK,"TCP encoded records use same validated reader");}
    {Fixture f(false,0,true);f.p.d.protocol=6;check(f.begin()==EJODIN4_DRIVER_UNSUPPORTED&&!f.s.state().any_write,"unknown TCP protocol fails before writes");}
    {Fixture f(false,0,true);f.p.d.protocol=4;FirmwareIdentity missing;check(f.begin(513,&missing)!=EJODIN4_OK&&!f.s.state().any_write,"TCP newer protocols retain authenticated identity gate");}
    {Fixture f(false,0,true);std::vector<uint8_t> candidate=f.p.d.pit;candidate[0]=0;check(f.begin(513,&identity,&candidate)!=EJODIN4_OK&&!f.p.d.commands&&!f.s.state().any_write,"TCP invalid PIT candidate is rejected before protocol I/O");}
    {Fixture f(false,0,true);check(f.begin(513)==EJODIN4_OK,"TCP failed ACK setup");f.p.d.wrong_index=0;PlanImage image=f.image(513);Reader raw(513);check(f.s.send_image(image,raw,f.error)==EJODIN4_INVALID_RESPONSE&&!f.s.state().acknowledged_bytes,"TCP wrong packet ACK credits no bytes");unsigned commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&f.p.d.commands==commands,"TCP failed writes are never replayed or finalized");}
}

void stock_encoded_tests(){
 {Fixture f;f.p.d.protocol=3;check(f.begin(513)==EJODIN4_OK&&f.s.compression_supported(),"native compression capability");size_t before=f.p.d.transcript.size();PlanImage image=f.image(513);Reader raw(513);raw.fail=true;EncodedReader encoded(513);check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_OK,"native encoded image avoids raw reader");check(f.p.d.transcript.size()==before+3,"encoded exact control count");const uint32_t subs[]={5,6,7};for(size_t i=0;i<3;++i)check(f.p.d.transcript[before+i][0]==0x66&&f.p.d.transcript[before+i][1]==subs[i],"native encoded begin/data/end sequence");const std::vector<uint32_t>& sizes=f.p.d.transcript[before+1];check(sizes[2]==73&&sizes[3]==513,"stored and decoded transfer lengths separate");const std::vector<uint32_t>& route=f.p.d.transcript[before+2];check(route[2]==0&&route[3]==513&&route[4]==0&&route[5]==2&&route[6]==1&&route[7]==1,"native encoded completion exact decoded length and PIT route");check(encoded.calls==2&&f.p.d.data_packet_sizes.size()==1&&f.p.d.data_packet_sizes[0]==1048576,"encoded EOF before padded packet");check(f.s.state().acknowledged_bytes==513&&f.s.state().images_completed==1,"decoded byte credit after completion");check(f.s.finish(false,f.error)==EJODIN4_OK,"encoded normal finalize");}
 {const uint64_t bytes=31457280u+513u;Fixture f;f.p.d.protocol=3;check(f.begin(bytes)==EJODIN4_OK,"encoded multi sequence begin");PlanImage image=f.image(bytes);Reader raw(bytes);EncodedReader encoded(bytes);encoded.record_sizes.push_back(1048579);encoded.record_sizes.push_back(73);check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_OK,"encoded multi sequence");check(f.p.d.encoded_completions.size()==2&&f.p.d.encoded_completions[0]==31457280&&f.p.d.encoded_completions[1]==513&&f.p.d.data_packet_sizes.size()==3,"encoded packet indices reset and exact logical sequence lengths");check(f.s.state().acknowledged_bytes==bytes&&f.s.finish(false,f.error)==EJODIN4_OK,"encoded multi complete");}
 for(unsigned mode=0;mode<3;++mode){Fixture f(mode==2);f.p.d.protocol=mode==0?2:3;if(mode==1)f.p.d.begin_capability=false;check(f.begin(513)==EJODIN4_OK&&!f.s.compression_supported(),"only native v3 low capability enables encoded");if(mode==2)check(f.s.notify_total(513,f.error)==EJODIN4_OK,"WinUSB encoded gate total");unsigned commands=f.p.d.commands;PlanImage image=f.image(513);Reader raw(513);EncodedReader encoded(513);check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_DRIVER_UNSUPPORTED&&f.p.d.commands==commands&&!f.s.state().any_write,"unnegotiated encoded cannot modify device");}
 for(unsigned mode=0;mode<7;++mode){Fixture f;f.p.d.protocol=3;check(f.begin(513)==EJODIN4_OK,"encoded prep failure begin");PlanImage image=f.image(513);Reader raw(513);EncodedReader encoded(513);if(mode==0)encoded.fail_call=1;else if(mode==1)encoded.fail_call=2;else if(mode==2)encoded.short_decoded=true;else if(mode==3)encoded.empty_records=true;else if(mode==4)encoded.oversized=true;else if(mode==5)encoded.extra=true;else encoded.cancel_device=&f.p.d;unsigned commands=f.p.d.commands;EJODIN4_Status s=f.s.send_image(image,raw,f.error,&encoded);check(s==(mode<2?EJODIN4_CHECKSUM_MISMATCH:mode==6?EJODIN4_CANCELLED:EJODIN4_INVALID_COMPRESSED_DATA),"encoded prep exact error");check(f.p.d.commands==commands&&!f.s.state().any_write&&!f.s.state().acknowledged_bytes,"encoded prep errors precede first modifying command");}
 for(unsigned mode=0;mode<7;++mode){Fixture f;f.p.d.protocol=3;check(f.begin(513)==EJODIN4_OK,"encoded wire failure begin");PlanImage image=f.image(513);Reader raw(513);EncodedReader encoded(513);if(mode==0)f.p.d.wrong_index=0;else if(mode==1)f.p.d.negative_data_ack=true;else if(mode==2)f.p.d.completion_fail=true;else if(mode==3)f.p.d.fail_write=true;else if(mode==4)f.p.d.cancel_encoded_packet=true;else{f.p.d.fail_command=0x66;f.p.d.fail_sub=mode==5?5:6;}EJODIN4_Status s=f.s.send_image(image,raw,f.error,&encoded);check(s==(mode==3?EJODIN4_USB_IO_ERROR:mode==4?EJODIN4_CANCELLED:EJODIN4_INVALID_RESPONSE),"encoded wire exact error");check(f.s.state().any_write&&!f.s.state().acknowledged_bytes&&!f.s.state().images_completed,"encoded completion failure cannot credit logical partition bytes");unsigned commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&f.p.d.commands==commands&&!f.p.d.finishes,"encoded failed state cannot finalize");for(size_t i=0;i<f.p.d.transcript.size();++i){const std::vector<uint32_t>& c=f.p.d.transcript[i];check(c[0]!=0x66||(c[1]!=0&&c[1]!=2&&c[1]!=3),"encoded failure never raw retries");}}
 {const uint64_t bytes=31457280u+513u;Fixture f;f.p.d.protocol=3;check(f.begin(bytes)==EJODIN4_OK,"late encoded failure begin");PlanImage image=f.image(bytes);Reader raw(bytes);EncodedReader encoded(bytes);encoded.fail_call=2;check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_CHECKSUM_MISMATCH,"late encoded reader error");check(f.s.state().acknowledged_bytes==31457280&&!f.s.state().images_completed&&f.p.d.encoded_completions.size()==1,"late failure credits only complete sequences");}
}

void happy(uint64_t bytes,bool modern,bool usb,bool extra,bool large,bool metadata_tail=false) {
    Fixture f(usb);f.p.d.protocol=modern?2:1;f.p.d.packet=modern?1048576:131072;f.p.d.optional_mode=extra?1:0;
    f.p.d.metadata_tail_bytes=metadata_tail?4:0;
    check(f.begin()==EJODIN4_OK,"begin session");check(f.pit.size()==1,"live PIT parsed");
    check(f.s.pit_bytes()==f.p.d.pit,"exact valid raw PIT retained");
    check(f.s.state().protocol_version==(modern?2u:1u),"unsigned compression bit masked");
    check(f.s.state().metadata_tail_present==(usb && metadata_tail),"metadata response framing established before images");
    if(usb && metadata_tail)check(f.s.state().metadata_tail_word==0x12345678u,"opaque device word retained");
    check(f.s.notify_total(bytes,f.error)==EJODIN4_OK,"64 bit total");check(f.p.d.total==bytes,"total high word");
    PlanImage image=f.image(bytes,large);Reader reader(bytes);
    check(f.s.send_image(image,reader,f.error)==EJODIN4_OK,"send image");
    check(f.s.state().acknowledged_bytes==bytes,"genuine ACK accounting");
    check(f.s.state().images_completed==1 && f.s.state().any_write,"completed image state");
    check(f.s.finish(true,f.error)==EJODIN4_OK,"finalize and reboot");
    check(f.s.state().finalized && f.s.state().reboot_sent,"final acknowledgement state");
    check(f.p.d.closes==(usb?0u:1u),"one serial cleanup");
    check(usb?f.p.d.zero_calls>0:f.p.d.zero_calls==0,"no serial USB ZLP");
    check(f.p.d.sequences==uint32_t((bytes+30u*1024u*1024u-1)/(30u*1024u*1024u)),"30 MiB sequences");
    unsigned resets=0;
    for(size_t rowIndex=0;rowIndex<f.p.d.transcript.size();++rowIndex) {const std::vector<uint32_t>& row=f.p.d.transcript[rowIndex]; if(row[0]==0x66 && row[1]==3) {
        check(row[4]==0 && row[5]==2 && row[6]==1,"device PIT route");
        check(row[8]==0 && row[9]==0,"no EFS clear or boot update");
        if(row[7])check(row[3]==(large && usb?uint32_t((bytes%(30u*1024u*1024u)+511)/512*512):uint32_t(bytes%(30u*1024u*1024u))),"last sequence real size");
    }}
    for(size_t rowIndex=0;rowIndex<f.p.d.transcript.size();++rowIndex) {const std::vector<uint32_t>& row=f.p.d.transcript[rowIndex];if(row[0]==0x64 && row[1]==1){++resets;check(row[2]==0,"empty reset counter payload");}}
    check(resets==(usb?2u:0u),"counter queries confined to pinned WinUSB flow");
}
typedef std::vector<std::vector<uint32_t> > Rows;
std::vector<uint32_t> row(uint32_t a,uint32_t b,uint32_t c=0,uint32_t d=0,uint32_t e=0,uint32_t f=0,uint32_t g=0,uint32_t h=0,uint32_t i=0,uint32_t j=0){const uint32_t values[]={a,b,c,d,e,f,g,h,i,j};return std::vector<uint32_t>(values,values+10);}
template<size_t N> Rows rows(const uint32_t (&values)[N][10]){Rows result;for(size_t i=0;i<N;++i)result.push_back(std::vector<uint32_t>(values[i],values[i]+10));return result;}
void repartition_session_tests(){
    FirmwareIdentity identity;identity.present=true;identity.model="SM-TEST_BUILD";
    for(unsigned network=0;network<2;++network){
    for(uint32_t version=1;version<=5;++version){
        Fixture f(false,0,network!=0);f.p.d.protocol=version;f.p.d.packet=version==1?131072:1048576;
        std::vector<uint8_t> candidate=f.p.d.pit;put(&candidate[0]+28+4,2);put(&candidate[0]+28+8,2);put(&candidate[0]+28+24,16);candidate.resize(663,0xc7);
        const std::vector<uint8_t> old=f.p.d.pit;f.p.d.pit_raw_fragment=version==1?1:37;
        check(f.begin(513,&identity,&candidate)==EJODIN4_OK,"repartition accepts changed candidate on each reviewed stock version");
        check(old!=candidate&&f.p.d.uploaded_pit==candidate&&f.p.d.pit_starts==1&&f.p.d.pit_uploads==1,"one exact unpadded PIT upload independent from old layout");
        Rows expected;expected.push_back(row(0x64,0,5));if(version>1)expected.push_back(row(0x64,5,1048576));expected.push_back(row(0x64,2,513));
        if(version>=4){expected.push_back(row(0x69,0));expected.push_back(row(0x69,1));expected.push_back(row(0x69,2));}
        expected.push_back(row(0x65,0));expected.push_back(row(0x65,2,663));expected.push_back(row(0x65,3,663));
        expected.push_back(row(0x65,1));expected.push_back(row(0x65,2,0));expected.push_back(row(0x65,2,1));expected.push_back(row(0x65,3));
        check(f.p.d.transcript==expected,"independent golden PIT upload and readback transcript");
        check(f.s.state().any_write&&f.s.state().pit_write_started&&f.s.state().pit_uploaded&&f.s.state().pit_verified,"PIT provenance follows independent acknowledgements and readback");
        check(!f.s.state().images_completed&&!f.s.state().acknowledged_bytes&&f.s.pit_bytes()==candidate&&f.pit[0].identifier==2,"PIT transfer never credits firmware bytes or image completion");
        PlanImage image=f.image(513);image.route=f.pit[0];Reader reader(513);
        check(f.s.send_image(image,reader,f.error)==EJODIN4_OK&&f.s.finish(false,f.error)==EJODIN4_OK,"image and checked finalize follow verified new layout");
        check(f.s.state().transport==(network?EJODIN4_DOWNLOAD_TCP:EJODIN4_DOWNLOAD_SAMSUNG_SERIAL)&&f.p.enumerations==(network?0u:1u)&&f.p.tcp_opens==(network?1u:0u),"repartition preserves selected transport and TCP bypasses USB enumeration");
        check(!f.p.d.zero_calls&&f.p.d.closes==1&&f.p.d.incoming.empty(),"native repartition closes once without USB boundaries or unread optional frames");
        for(size_t k=0;k<f.p.d.transcript.size();++k)check(!(f.p.d.transcript[k][0]==0x64&&f.p.d.transcript[k][1]==1),"serial and TCP repartition never send WinUSB counter resets");
    }
    const unsigned ack_stages[]={0,1,2,3,4,6};
    for(size_t k=0;k<sizeof(ack_stages)/sizeof(ack_stages[0]);++k)for(unsigned mode=1;mode<=4;++mode){
        Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;f.p.d.pit_fault_stage=ack_stages[k];f.p.d.pit_fault_mode=mode;
        EJODIN4_Status status=f.begin(513,&identity,&candidate);
        check(status==(mode==3?EJODIN4_TIMEOUT:mode==4?EJODIN4_CANCELLED:EJODIN4_INVALID_RESPONSE),"every PIT ACK rejects negative, foreign, partial and cancelled reply");
        check(f.s.state().any_write&&f.s.state().pit_write_started&&!f.s.state().pit_verified,"PIT failure preserves possible side effect without readback proof");
        check(f.s.state().pit_uploaded==(ack_stages[k]>=4),"upload proof requires successful final upload ACK");
        const uint32_t commands=f.p.d.commands;PlanImage image=f.image(513);Reader reader(513);
        check(f.s.send_image(image,reader,f.error)!=EJODIN4_OK&&f.s.finish(true,f.error)!=EJODIN4_OK&&commands==f.p.d.commands,"PIT ACK failure sends no image, next command, finalize or reboot");
        check(!f.p.d.images&&!f.p.d.finishes&&!f.s.state().acknowledged_bytes&&!f.s.state().images_completed,"PIT ACK failures retain zero image counters");
        const uint32_t last_sub=f.p.d.transcript.back()[1];check(last_sub==(ack_stages[k]==0?0:ack_stages[k]<=2?2:ack_stages[k]==4?1:3),"failed stage is final transmitted control");
    }
    for(unsigned stage=0;stage<=3;++stage){Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;f.p.d.pit_fault_stage=stage;f.p.d.pit_fault_mode=5;
        check(f.begin(513,&identity,&candidate)==EJODIN4_OK,"positive nonzero PIT upload ACK remains valid");check(f.s.state().pit_verified,"positive PIT upload ACK still requires full readback");}
    const uint32_t limits[]={0,1,79,159};
    for(size_t k=0;k<sizeof(limits)/sizeof(limits[0]);++k)for(unsigned mode=0;mode<3;++mode){Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;
        f.p.d.pit_raw_limit=limits[k];f.p.d.pit_raw_failure=mode==0?EJODIN4_USB_IO_ERROR:mode==1?EJODIN4_TIMEOUT:EJODIN4_CANCELLED;
        check(f.begin(513,&identity,&candidate)==f.p.d.pit_raw_failure,"short raw PIT transfer preserves terminal error");
        check(f.p.d.write_buffer.size()==limits[k]&&f.p.d.pit_starts==1&&!f.p.d.pit_uploads&&!f.s.state().pit_uploaded&&!f.s.state().pit_verified&&f.s.state().any_write,"short PIT write retains exact prefix and conservative side effect");
        check(f.p.d.transcript.back()==row(0x65,2,160)&&f.p.d.uploaded_pit.empty(),"short raw PIT never emits upload final or retries");
        unsigned commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&commands==f.p.d.commands,"short PIT write failure blocks finalize");}
    for(unsigned mode=0;mode<4;++mode){Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;put(&candidate[0]+28+8,2);
        if(mode==0)f.p.d.pit_keep_old=true;else{f.p.d.pit_fault_stage=5;f.p.d.pit_fault_mode=mode==1?3:mode==2?4:1;}
        check(f.begin(513,&identity,&candidate)!=EJODIN4_OK,"mismatched, short, cancelled or malformed PIT readback rejects");
        check(f.s.state().pit_uploaded&&!f.s.state().pit_verified&&f.s.state().any_write&&!f.p.d.images&&!f.p.d.finishes,"failed readback retains upload proof and zero images");
        const unsigned commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&commands==f.p.d.commands,"failed PIT readback cannot finalize or reboot");}
    for(unsigned mode=0;mode<5;++mode){Fixture f(mode==0,0,network!=0&&mode!=0);std::vector<uint8_t> candidate=f.p.d.pit;const FirmwareIdentity* context=&identity;uint64_t total=513;
        if(mode==1)candidate[0]=0;if(mode==2)candidate.clear();if(mode==3)context=0;if(mode==4)total=0;
        check(f.begin(total,context,&candidate)!=EJODIN4_OK,"invalid repartition context is rejected before handshake");
        check(f.p.d.expected==4&&!f.p.d.commands&&!f.p.d.pit_starts&&!f.s.state().any_write,"invalid candidate, transport, identity context or total has zero protocol I/O");}
    for(uint32_t version=4;version<=5;++version){Fixture f(false,0,network!=0);f.p.d.protocol=version;FirmwareIdentity missing;std::vector<uint8_t> candidate=f.p.d.pit;
        check(f.begin(513,&missing,&candidate)!=EJODIN4_OK&&!f.p.d.pit_starts&&!f.s.state().any_write,"protocol4/5 missing authenticated identity precedes PIT upload");
        for(size_t k=0;k<f.p.d.transcript.size();++k)check(f.p.d.transcript[k][0]!=0x65,"missing identity never emits a PIT command");}
    for(unsigned mode=0;mode<2;++mode){Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;if(mode==0)candidate.resize(663,0x93);f.p.d.pit_readback_size=mode==0?160:663;
        check(f.begin(513,&identity,&candidate)==EJODIN4_OK&&f.s.state().pit_verified,"PIT readback equality ignores different raw padding lengths and values");
        check(f.p.d.uploaded_pit==candidate&&f.s.pit_bytes().size()==f.p.d.pit_readback_size,"candidate wire bytes and actual readback evidence stay exact");}
    if(!network)for(unsigned mode=1;mode<=4;++mode){Fixture f(false,0,network!=0);std::vector<uint8_t> candidate=f.p.d.pit;f.p.d.optional_mode=mode;
        const EJODIN4_Status status=f.begin(513,&identity,&candidate);
        check((status==EJODIN4_OK)==(mode==1),"optional postupload PIT frame retains strict normal download semantics");
        check(f.s.state().pit_uploaded&&f.s.state().pit_verified==(mode==1),"optional PIT frame failure cannot manufacture verified proof");
        if(mode!=1){const unsigned commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&commands==f.p.d.commands&&f.p.d.transcript.back()==row(0x65,2),"failed optional PIT frame stops before download final and reboot");}}
    {Fixture f(false,0,network!=0);f.p.d.protocol=3;std::vector<uint8_t> candidate=f.p.d.pit;put(&candidate[0]+28+4,2);
        check(f.begin(513,&identity,&candidate)==EJODIN4_OK&&f.s.state().pit_verified&&f.s.compression_supported(),"repartition retains negotiated compression only after verified PIT");
        PlanImage image=f.image(513);image.route=f.pit[0];Reader raw(513);raw.fail=true;EncodedReader encoded(513);
        check(f.s.send_image(image,raw,f.error,&encoded)==EJODIN4_OK&&f.s.state().acknowledged_bytes==513&&f.s.state().images_completed==1&&f.s.finish(false,f.error)==EJODIN4_OK,"encoded image and checked finalize follow verified repartition without raw fallback");}
    }
    for(uint32_t version=1;version<=5;++version){Fixture f(false,0,true);f.p.d.protocol=version;f.p.d.packet=version==1?131072:1048576;f.p.d.pit_raw_ack_id=0;
        std::vector<uint8_t> candidate=f.p.d.pit;
        check(f.begin(513,&identity,&candidate)==EJODIN4_OK&&f.s.state().pit_uploaded&&f.s.state().pit_verified,"TCP raw PIT data permits generic data ACK on every supported protocol");
        check(f.p.d.uploaded_pit==candidate&&!f.s.state().acknowledged_bytes&&f.s.close(f.error)==EJODIN4_OK,"generic TCP data ACK still requires exact PIT readback and credits no firmware");}
    for(unsigned mode=0;mode<3;++mode){Fixture f(false,0,true);f.p.d.pit_raw_ack_id=0;f.p.d.pit_fault_stage=2;f.p.d.pit_fault_mode=mode==0?1:mode==1?2:3;
        std::vector<uint8_t> candidate=f.p.d.pit;
        check(f.begin(513,&identity,&candidate)==(mode==2?EJODIN4_TIMEOUT:EJODIN4_INVALID_RESPONSE)&&!f.s.state().pit_uploaded&&!f.s.state().pit_verified,"generic TCP raw data ACK rejects negative values, foreign IDs and short replies");}
    {Fixture f;f.p.d.pit_raw_ack_id=0;std::vector<uint8_t> candidate=f.p.d.pit;
        check(f.begin(513,&identity,&candidate)==EJODIN4_INVALID_RESPONSE&&!f.s.state().pit_uploaded,"generic raw PIT data ACK remains forbidden on Samsung serial");}
}

void stock_raw_golden() {
    struct TotalVector { uint64_t total; uint32_t remainder,quotient; }; const TotalVector totals[]={
        {UINT64_C(4294967295),0,1},{UINT64_C(4294967296),1,1},
        {UINT64_C(8589934590),0,2},{UINT64_C(11882352976),0xc43e5152u,2},
        {UINT64_C(1099511627776),256,256}};
    for(size_t vector_index=0;vector_index<sizeof(totals)/sizeof(totals[0]);++vector_index) { const TotalVector& vector=totals[vector_index];
        Fixture f;
        check(f.begin(vector.total)==EJODIN4_OK,"native stock discovery begins with planned total");
        const uint32_t expected_values[][10]={
            {0x64,0,5,0,0,0,0,0,0,0},{0x64,5,1048576,0,0,0,0,0,0,0},
            {0x64,2,vector.remainder,vector.quotient,0,0,0,0,0,0},
            {0x65,1,0,0,0,0,0,0,0,0},{0x65,2,0,0,0,0,0,0,0,0},
            {0x65,3,0,0,0,0,0,0,0,0}};
        const Rows expected=rows(expected_values);
        check(f.p.d.transcript==expected,"native protocol5 base-UINT32_MAX total precedes PIT without counter query");
        check(f.p.d.total==vector.total && !f.s.state().any_write,"pre-PIT transfer metadata does not claim partition writes");
        check(f.s.notify_total(1,f.error)==EJODIN4_INVALID_ARGUMENT,"pre-PIT total cannot be announced twice");
        check(f.s.close(f.error)==EJODIN4_OK,"golden metadata-only transport cleanup");
    }
    struct RawVector { uint64_t bytes; uint32_t first_start,tail_start,tail_real,packets; bool large; }; const RawVector cases[]={
        {131073,262144,262144,131073,1,false},
        {1048576,1048576,1048576,1048576,1,false},
        {1048577,1179648,1179648,1048577,2,false},
        {513,131072,131072,513,1,true},
        {UINT64_C(31588353),31457280,262144,131073,31,true},
        {UINT64_C(33232988),31457280,1835008,1775708,32,true}};
    for(size_t vector_index=0;vector_index<sizeof(cases)/sizeof(cases[0]);++vector_index) { const RawVector& vector=cases[vector_index];
        Fixture f;check(f.begin(vector.bytes)==EJODIN4_OK,"native raw golden session begins");
        const size_t before=f.p.d.transcript.size();
        PlanImage image=f.image(vector.bytes,vector.large);
        const bool super_tail=vector.tail_real==1775708;
        if(vector.bytes>31457280){image.route.device_type=8;image.route.identifier=super_tail?31u:32u;}
        Reader reader(vector.bytes);
        check(f.s.send_image(image,reader,f.error)==EJODIN4_OK,"native raw golden image transfers");
        const bool multiple=vector.bytes>31457280;
        Rows expected;
        if(multiple) {
            expected.push_back(row(0x66,0,0,0,0,0,0,0,0,0));
            expected.push_back(row(0x66,2,vector.first_start,0,0,0,0,0,0,0));
            expected.push_back(super_tail?row(0x66,3,0,31457280,0,8,31,0,0,0):
                row(0x66,3,0,31457280,0,8,32,0,0,0));
        }
        expected.push_back(row(0x66,0,0,0,0,0,0,0,0,0));
        expected.push_back(row(0x66,2,vector.tail_start,0,0,0,0,0,0,0));
        if(super_tail) expected.push_back(row(0x66,3,0,1775708,0,8,31,1,0,0));
        else if(multiple) expected.push_back(row(0x66,3,0,131073,0,8,32,1,0,0));
        else expected.push_back(row(0x66,3,0,vector.tail_real,0,2,1,1,0,0));
        check(Rows(f.p.d.transcript.begin()+before,f.p.d.transcript.end())==expected,"native golden sequence begins repeat, start aligns128KiB and completion is unrounded");
        check(f.p.d.data_packet_sizes==std::vector<uint32_t>(vector.packets,1048576),"native actual data remains full negotiated one-MiB packets");
        check(f.s.state().acknowledged_bytes==vector.bytes && f.s.state().images_completed==1,"native progress counts only actual decoded bytes");
        check(f.s.finish(false,f.error)==EJODIN4_OK,"native golden finishes once after exact byte ACKs");
    }
    Fixture partial;check(partial.begin(131073)==EJODIN4_OK,"partial native data ACK fixture begins");
    PlanImage image=partial.image(131073);Reader reader(131073);partial.p.d.data_ack_bytes=3;
    check(partial.s.send_image(image,reader,partial.error)==EJODIN4_TIMEOUT,"incomplete native packet ACK fails closed");
    check(!partial.s.state().acknowledged_bytes && !partial.s.state().images_completed,"incomplete ACK advances no byte or image progress");
    const uint32_t sent=partial.p.d.commands;
    check(partial.s.finish(true,partial.error)!=EJODIN4_OK && partial.p.d.commands==sent && !partial.p.d.finishes,"incomplete native ACK sends no completion or reboot");
}

void native_devinfo_versions(){
 for(uint32_t version=4;version<=5;++version){FirmwareIdentity identity;identity.present=true;identity.model="SM-TEST_BUILD";
 Fixture f;f.p.d.protocol=version;check(f.begin(513,&identity)==EJODIN4_OK,"native v4/v5 authenticates model before PIT");
 Rows prefix;prefix.push_back(row(0x64,0,5));prefix.push_back(row(0x64,5,1048576));prefix.push_back(row(0x64,2,513));prefix.push_back(row(0x69,0));prefix.push_back(row(0x69,1));prefix.push_back(row(0x69,2));prefix.push_back(row(0x65,1));prefix.push_back(row(0x65,2));prefix.push_back(row(0x65,3));check(f.p.d.transcript==prefix,"native v4/v5 exact DEVINFO stage ordering");
 PlanImage image=f.image(513);Reader reader(513);check(f.s.send_image(image,reader,f.error)==EJODIN4_OK&&f.s.finish(false,f.error)==EJODIN4_OK,"authenticated newer native raw completes");
 Fixture readonly;readonly.p.d.protocol=version;check(readonly.begin()==EJODIN4_OK,"newer PIT read has no firmware authority requirement");check(readonly.s.notify_total(513,readonly.error)==EJODIN4_OK,"newer read only session records total");image=readonly.image(513);Reader denied(513);check(readonly.s.send_image(image,denied,readonly.error)==EJODIN4_DRIVER_UNSUPPORTED&&!readonly.s.state().any_write,"read only session cannot acquire unauthenticated write authority");
 for(unsigned mode=0;mode<9;++mode){Fixture bad;bad.p.d.protocol=version;FirmwareIdentity context=identity;if(mode==0)context.present=false;if(mode==1)context.model="SM-OTHER";if(mode==2)context.blocked_carriers="_PLS_";if(mode==3)bad.p.d.devinfo_size_override=499;if(mode==4)put(&bad.p.d.devinfo[0],0);if(mode==5)put(&bad.p.d.devinfo[0]+12,0xffffffffu);if(mode==6)bad.p.d.devinfo_short=true;if(mode==7)bad.p.d.wrong_command=0x69;if(mode==8){context.present=false;context.status=EJODIN4_CHECKSUM_MISMATCH;context.diagnostic="Signed manifest authentication failed";}check(bad.begin(513,&context)!=EJODIN4_OK,"malformed DEVINFO or invalid package identity rejects");check(!bad.s.state().any_write&&!bad.p.d.images&&!bad.p.d.finishes,"DEVINFO rejection precedes all writes");for(size_t i=0;i<bad.p.d.transcript.size();++i){const std::vector<uint32_t>& row=bad.p.d.transcript[i];check(row[0]!=0x65&&row[0]!=0x66,"identity rejection precedes PIT and image commands");}}
 }
 Fixture missing;missing.p.d.protocol=5;put(&missing.p.d.devinfo[0]+4,1);check(missing.begin()!=EJODIN4_OK&&!missing.s.state().any_write,"v5 missing carrier rejected during readonly discovery");
}

void stock_versions_and_routes() {
    const uint32_t rejected[]={0,4,5,6,0x8002,0xffff};
    for(unsigned usb=0;usb<2;++usb)for(size_t k=0;k<sizeof(rejected)/sizeof(rejected[0]);++k) {
        if((!usb&&(rejected[k]==4||rejected[k]==5))||(usb&&k>=4))continue;
        Fixture f(usb!=0);f.p.d.protocol=rejected[k];
        check(f.begin(513)==EJODIN4_DRIVER_UNSUPPORTED,"unknown transport dialect rejected before PIT metadata");
        Rows expected;expected.push_back(row(0x64,0,usb?0x7fffffffu:5u));check(f.p.d.transcript==expected,"unknown dialect negotiation only");
        check(f.s.state().protocol_version==rejected[k]&&!f.s.state().any_write&&!f.p.d.finishes,"unknown version retains no write evidence");
        uint32_t commands=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK&&commands==f.p.d.commands,"unknown version cannot finalize");
    }
    for(uint32_t version=1;version<=3;++version)for(unsigned capability=0;capability<2;++capability) {
        Fixture f;f.p.d.protocol=version;f.p.d.packet=version==1?131072:1048576;f.p.d.begin_capability=capability!=0;
        check(f.begin(513)==EJODIN4_OK,"reviewed raw dialect begins");
        Rows expected;expected.push_back(row(0x64,0,5));if(version!=1)expected.push_back(row(0x64,5,1048576));
        expected.push_back(row(0x64,2,513));expected.push_back(row(0x65,1));expected.push_back(row(0x65,2));expected.push_back(row(0x65,3));
        check(f.p.d.transcript==expected,"accepted stock dialect total precedes PIT without query");
        check(f.s.state().negotiation_ack==((version<<16)|(capability?0x8000u:0u)),"stock negotiation capability remains available after PIT ACKs");
        const size_t before=f.p.d.transcript.size();PlanImage image=f.image(513,true);Reader reader(513);
        check(f.s.send_image(image,reader,f.error)==EJODIN4_OK,"accepted stock dialect sends raw tail");
        const uint32_t image_values[][10]={{0x66,0,0,0,0,0,0,0,0,0},{0x66,2,131072,0,0,0,0,0,0,0},{0x66,3,0,513,0,2,1,1,0,0}};
        check(Rows(f.p.d.transcript.begin()+before,f.p.d.transcript.end())==rows(image_values),"legacy and modern raw completion keep exact513 bytes");
        check(f.p.d.data_packet_sizes==std::vector<uint32_t>(1,version==1?131072u:1048576u),"data writes retain negotiated legacy or modern packet size");
        check(f.s.finish(false,f.error)==EJODIN4_OK,"accepted raw dialect finalizes");
    }
    const struct {uint32_t type,cap;} routes[]={{1,31457280},{2,31457280},{8,31457280},{6,4194304},{7,1048576}};
    for(uint32_t version=1;version<=3;++version)for(size_t k=0;k<sizeof(routes)/sizeof(routes[0]);++k)for(int delta=-1;delta<=1;++delta) {
        const uint32_t cap=routes[k].cap,type=routes[k].type;
        const uint64_t bytes=delta<0?cap-1:delta>0?cap+1:cap;
        Fixture f;f.p.d.protocol=version;f.p.d.packet=version==1?131072:1048576;
        check(f.begin(bytes)==EJODIN4_OK,"stock route cap fixture begins");PlanImage image=f.image(bytes,true);image.route.device_type=type;Reader reader(bytes);
        const size_t before=f.p.d.transcript.size();check(f.s.send_image(image,reader,f.error)==EJODIN4_OK,"reviewed route cap transfers");
        Rows expected;
        if(delta>0){expected.push_back(row(0x66,0));expected.push_back(row(0x66,2,cap));expected.push_back(row(0x66,3,0,cap,0,type,1,0));}
        expected.push_back(row(0x66,0));expected.push_back(row(0x66,2,delta>0?131072u:cap));expected.push_back(row(0x66,3,0,delta>0?1u:uint32_t(bytes),0,type,1,1));
        check(Rows(f.p.d.transcript.begin()+before,f.p.d.transcript.end())==expected,"route-specific30/4/1MiB cap and every-sequence begin match literal expectations");
        check(f.s.state().acknowledged_bytes==bytes && f.s.finish(false,f.error)==EJODIN4_OK,"route cap preserves real-byte ACK progress");
    }
    const uint32_t unsupported[]={0,3,4,5,9,0xffffffffu};
    for(size_t k=0;k<sizeof(unsupported)/sizeof(unsupported[0]);++k){Fixture f;check(f.begin(10)==EJODIN4_OK,"unsupported route fixture begins");PlanImage image=f.image(10);image.route.device_type=unsupported[k];Reader reader(10);const uint32_t before=f.p.d.commands;
        check(f.s.send_image(image,reader,f.error)==EJODIN4_DRIVER_UNSUPPORTED && f.p.d.commands==before && !f.s.state().any_write,"unreviewed stock route fails before image begin");
        check(f.s.finish(true,f.error)!=EJODIN4_OK && f.p.d.commands==before,"unreviewed route cannot finalize");}
    {Fixture f;check(f.begin(10)==EJODIN4_OK,"stock modem route fixture begins");PlanImage image=f.image(10);image.route.binary_type=1;Reader reader(10);const uint32_t before=f.p.d.commands;
        check(f.s.send_image(image,reader,f.error)==EJODIN4_DRIVER_UNSUPPORTED && f.p.d.commands==before && !f.s.state().any_write,"unreviewed stock modem path fails before dispatch");}
    {Fixture f(true);f.p.d.protocol=3;check(f.begin()==EJODIN4_OK,"WinUSB protocol3 retained");check(f.s.notify_total(513,f.error)==EJODIN4_OK,"WinUSB total remains post-PIT");PlanImage image=f.image(513,true);Reader reader(513);
        check(f.s.send_image(image,reader,f.error)==EJODIN4_OK && f.s.finish(false,f.error)==EJODIN4_OK,"WinUSB protocol3 retains pinned flow");
        const uint32_t values[][10]={{0x64,0,0x7fffffffu,0,0,0,0,0,0,0},{0x64,5,1048576,0,0,0,0,0,0,0},{0x65,1,0,0,0,0,0,0,0,0},{0x65,2,0,0,0,0,0,0,0,0},{0x65,3,0,0,0,0,0,0,0,0},{0x64,1,0,0,0,0,0,0,0,0},{0x64,2,513,0,0,0,0,0,0,0},{0x66,0,0,0,0,0,0,0,0,0},{0x66,2,1048576,0,0,0,0,0,0,0},{0x66,3,0,1024,0,2,1,1,0,0},{0x64,1,0,0,0,0,0,0,0,0},{0x67,0,0,0,0,0,0,0,0,0}};
        check(f.p.d.transcript==rows(values),"WinUSB alignment, rounding, queries and total order remain pinned");}
}
void handshake_diagnostics() {
    for(unsigned mode=0;mode<4;++mode) {
        Fixture f;
        if(mode==0) f.p.d.handshake_write_fail_after=2;
        else f.p.d.handshake_read_fail_after=mode==1?0:2;
        if(mode==3)f.p.d.handshake_failure=EJODIN4_CANCELLED;
        const EJODIN4_Status result=f.begin();
        check(result==(mode==3?EJODIN4_CANCELLED:EJODIN4_TIMEOUT), "Handshake terminal status is preserved");
        check(f.error.find(mode==0?"ODIN handshake write failed":"Waiting for LOKE reply failed")!=std::string::npos,"Handshake error identifies ODIN write versus LOKE wait");
        check(f.error.find(mode==0?"write stopped after 2/4":mode==1?"read stopped after 0/4":"read stopped after 2/4")!=std::string::npos,"Handshake error records cumulative completed bytes");
        check(f.error.find("terminal Windows error 995")!=std::string::npos,"Native error context survives session diagnostic wrapping");
        check(!f.s.state().protocol_version && !f.s.state().any_write && !f.s.state().acknowledged_bytes,"Failed handshake cannot claim negotiation or partition writes");
        check(!f.p.d.commands && !f.p.d.finishes,"Handshake failure sends no control commands or finalization");
        check(f.p.d.control_budgets.size()==1 && f.p.d.control_budgets[0]==30000,"Handshake retains default 30s deadline before modern negotiation");
        check(mode==0?f.p.d.write_buffer.size()==2:f.p.d.handshake_read_bytes==(mode==1?0u:2u),"Handshake failure neither resends ODIN nor consumes further reply bytes");
        const size_t written=f.p.d.write_buffer.size();
        check(f.s.begin(f.pit,f.error)==EJODIN4_INVALID_ARGUMENT && f.p.d.write_buffer.size()==written && !f.p.d.commands,"Failed session cannot automatically begin or resend handshake");
    }
}
void delayed_data_ack_tests(){
    const uint32_t timeouts[]={0u,30000u}; for(size_t timeoutIndex=0;timeoutIndex<2;++timeoutIndex){uint32_t timeout=timeouts[timeoutIndex];
        Fixture f(false,timeout);
        check(f.begin()==EJODIN4_OK,"modern session begins before delayed ACK regression");
        check(f.p.d.protocol==2&&f.p.d.packet==1048576,"delayed ACK test retains negotiated one-MiB protocol packets");
        check(f.p.open_budget_ms==30000,"default and explicit data-timeout configurations retain a thirty-second device-open budget");
        check(!f.p.d.control_budgets.empty()&&size_t(std::count(f.p.d.control_budgets.begin(),f.p.d.control_budgets.end(),uint64_t(30000)))==f.p.d.control_budgets.size(),"handshake and initial control commands retain thirty-second deadlines");
        const uint64_t bytes=513;
        check(f.s.notify_total(bytes,f.error)==EJODIN4_OK,"delayed ACK test advertises genuine image byte count");
        f.p.d.data_ack_delay_ms=45000;PlanImage image=f.image(bytes);Reader reader(bytes);
        const EJODIN4_Status status=f.s.send_image(image,reader,f.error);
        check(f.p.d.data_write_budget_ms==(timeout?30000u:120000u)&&f.p.d.data_ack_budget_ms==(timeout?30000u:120000u),"modern writes and data ACKs use the negotiated default unless explicitly overridden");
        if(!timeout){
            check(status==EJODIN4_OK&&f.s.state().acknowledged_bytes==bytes&&f.s.state().images_completed==1,"forty-five-second data ACK succeeds within the default modern two-minute budget");
            check(f.s.finish(false,f.error)==EJODIN4_OK,"delayed successful ACK permits checked session finalization");
        }else{
            check(status==EJODIN4_TIMEOUT&&f.s.state().acknowledged_bytes==0&&f.s.state().images_completed==0&&f.s.state().any_write,"explicit thirty-second fails without false ACKs after a potentially issued write");
            const uint32_t commands=f.p.d.commands;
            check(f.s.finish(true,f.error)!=EJODIN4_OK&&f.p.d.commands==commands&&!f.p.d.finishes,"timed-out data ACK cannot finalize reboot or send more commands");
        }
        check(f.p.d.zero_calls==0,"timeout correction never adds serial USB boundary packets");
    }
}
void failures() {
    {Fixture f;f.p.duplicate=true;check(f.begin()==EJODIN4_DRIVER_UNSUPPORTED,"duplicate exact interface");check(!f.p.d.commands,"duplicate sends nothing");}
    {Fixture f;f.p.missing=true;check(f.begin()==EJODIN4_NO_DEVICE,"missing selected interface");}
    {Fixture f;check(f.c.open("COM16",6,30000,f.error)==EJODIN4_NO_DEVICE,"no arbitrary COM alias");}
    {Fixture f;f.p.d.handshake_bad=true;check(f.begin()==EJODIN4_BAD_HANDSHAKE,"bad handshake");}
    {Fixture f;f.p.d.wrong_command=0x64;check(f.begin()==EJODIN4_INVALID_RESPONSE,"wrong control id");}
    {Fixture f;f.p.d.pit_size_override=1048577;check(f.begin()!=EJODIN4_OK,"bounded PIT size");}
    {Fixture f;f.p.d.pit[0]=0;check(f.begin()==EJODIN4_INVALID_PIT,"invalid raw PIT signature");check(f.s.pit_bytes().empty(),"invalid PIT exposes no raw evidence");}
    {Fixture f;f.p.d.pit_short=true;check(f.begin()==EJODIN4_TIMEOUT,"short raw PIT fails");check(f.s.pit_bytes().empty(),"failed PIT read exposes no raw evidence");}
    for(unsigned mode=2;mode<=4;++mode){Fixture f;f.p.d.optional_mode=mode;check(f.begin()!=EJODIN4_OK,"partial foreign failed optional frame");check(!f.p.d.finishes,"no end after PIT error");}
    for(unsigned size=1;size<4;++size){Fixture f(true);f.p.d.metadata_tail_bytes=size;check(f.begin()!=EJODIN4_OK && !f.s.state().any_write,"partial metadata frame blocks images");}
    {Fixture f;f.p.d.pit.resize(160+500,0);f.p.d.pit.back()=0x93;check(f.begin()==EJODIN4_OK,"PIT multi block final short raw bytes");check(f.s.pit_bytes()==f.p.d.pit,"raw PIT signature and trailing bytes retained exactly");check(f.s.finish(false,f.error)==EJODIN4_OK,"discovery close without image or reboot");check(!f.s.state().any_write && !f.s.state().reboot_sent,"discovery truthful");}
    {Fixture f;f.p.d.pit.resize(1000,0);check(f.begin()==EJODIN4_OK,"PIT exact 500 byte block multiple");}
    {Fixture f(true);check(f.begin()==EJODIN4_OK,"begin modem binary route");f.s.notify_total(10,f.error);PlanImage i=f.image(10);i.route.binary_type=1;i.route.identifier=99;Reader r(10);check(f.s.send_image(i,r,f.error)==EJODIN4_OK,"modem route transfer");for(size_t rowIndex=0;rowIndex<f.p.d.transcript.size();++rowIndex) {const std::vector<uint32_t>& row=f.p.d.transcript[rowIndex];if(row[0]==0x66 && row[1]==3)check(row[2]==1 && row[4]==1 && row[6]==0,"modem destination omits partition identifier");}}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin wrong index case");check(f.s.notify_total(10,f.error)==EJODIN4_OK,"set total");f.p.d.wrong_index=0;PlanImage i=f.image(10);Reader r(10);check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"wrong packet index");check(f.s.state().acknowledged_bytes==0 && f.s.state().any_write,"uncertain write truthful");const uint32_t n=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK && f.p.d.commands==n,"no commands after error");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin completion failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.completion_fail=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"negative -2 not progress");check(f.s.state().acknowledged_bytes==10 && !f.s.state().images_completed,"packet ACK does not mean image complete");}
    {Fixture f(true);check(f.begin()==EJODIN4_OK,"begin reset failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.fail_reset=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"reset counter strict ACK");check(f.s.state().images_completed==1 && f.s.state().acknowledged_bytes==10,"written image retained after reset failure");const uint32_t n=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK && f.p.d.commands==n,"reset failure stops reboot and next commands");}
    {Fixture f(true);check(f.begin()==EJODIN4_OK,"begin framing change");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.change_metadata_framing=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"changed reset framing fails");check(f.s.state().images_completed==1 && !f.s.state().reboot_sent,"framing error retains completed image");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin uncertain write");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.fail_write=true;check(f.s.send_image(i,r,f.error)==EJODIN4_USB_IO_ERROR,"failed packet write");check(f.s.state().any_write && f.s.state().acknowledged_bytes==0,"uncertain packet excluded from ACK total");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin file request failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.fail_command=0x66;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"file begin ACK failure");check(f.s.state().any_write && !f.s.state().acknowledged_bytes && !f.s.state().images_completed,"file begin conservatively marks possible partition change");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin decode failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);r.fail=true;const uint32_t n=f.p.d.commands;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_COMPRESSED_DATA,"prefill decode failure");check(f.p.d.commands==n && !f.s.state().any_write,"decode before XMITSTART");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin extra byte");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);r.too_long=true;check(f.s.send_image(i,r,f.error)!=EJODIN4_OK && !f.s.state().any_write,"EOF checked before final sequence");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin cancellation");f.p.d.cancelled=true;check(f.s.notify_total(10,f.error)==EJODIN4_CANCELLED,"cancel before request");check(!f.s.state().any_write,"cancel before writes truthful");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin cleanup failure");f.p.d.fail_close=true;check(f.s.finish(false,f.error)==EJODIN4_USB_IO_ERROR,"cleanup failure prevents success");check(f.s.state().phase==EJODIN4_PHASE_FAILED,"cleanup failure phase");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin final ACK failure");f.p.d.fail_command=0x67;check(f.s.finish(true,f.error)==EJODIN4_INVALID_RESPONSE,"negative final ACK");check(!f.s.state().finalized && !f.s.state().reboot_sent,"failed final ACK does not claim completion");check(f.p.d.finishes==1,"no reboot after failed finalize");}
    for(unsigned mode=0;mode<4;++mode) {
        Fixture f;check(f.c.open("bound-interface",16,30000,f.error)==EJODIN4_OK,"open zero read test");
        f.p.d.incoming.push_back(0x5a);f.p.d.read_zeroes=2;f.p.d.zero_advance=mode==0?30000:mode==1?60000:0;f.p.d.zero_cancel=mode==3;
        uint8_t b=0;const EJODIN4_Status result=f.c.read_exact(&b,1,120000,f.error);
        check(mode==0?result==EJODIN4_OK && b==0x5a:mode==1?result==EJODIN4_TIMEOUT:mode==3?result==EJODIN4_CANCELLED:result==EJODIN4_USB_IO_ERROR,"bounded zero read retry");
    }
}

struct OperationEvents {
    bool cancelled;uint32_t throw_phase,cancel_phase,pit_events,complete_events;std::vector<uint32_t> phases;
    OperationEvents():cancelled(false),throw_phase(0),cancel_phase(0),pit_events(0),complete_events(0){}
};
int32_t EJODIN4_CALL operation_cancel(void* context){return static_cast<OperationEvents*>(context)->cancelled?1:0;}
void EJODIN4_CALL operation_progress(const EJODIN4_FlashProgress* event,void* context){
    OperationEvents& events=*static_cast<OperationEvents*>(context);
    check(event&&event->struct_size==sizeof(*event),"Integrated operation callback uses packed native layout");
    events.phases.push_back(event->phase);
    if(event->phase==EJODIN4_PHASE_PIT_ENTRY){++events.pit_events;check(event->partition.identifier==1&&!std::strcmp(event->partition.partition_name,"BOOT"),"Integrated PIT callback retains actual parsed route");}
    if(event->phase==EJODIN4_PHASE_COMPLETE)++events.complete_events;
    if(event->phase==events.cancel_phase)events.cancelled=true;
    if(event->phase==events.throw_phase)throw std::runtime_error("injected progress failure");
}
EJODIN4_FlashRequest read_pit_request(){EJODIN4_FlashRequest r;std::memset(&r,0,sizeof(r));r.struct_size=sizeof(r);r.operation=EJODIN4_FLASH_READ_PIT;r.device_path="bound-interface";r.device_path_bytes=16;return r;}
typedef std::vector<uint8_t> Bytes;
static void append_package(Bytes& tar,const char* name,const Bytes& data){
    Bytes header(512,0);std::memcpy(&header[0],name,std::strlen(name));
    uint64_t size=data.size();for(int i=134;i>=124;--i){header[size_t(i)]=uint8_t('0'+(size&7));size>>=3;}
    header[156]='0';std::memcpy(&header[257],"ustar",5);std::fill(header.begin()+148,header.begin()+156,uint8_t(' '));
    unsigned sum=0;for(size_t i=0;i<header.size();++i)sum+=header[i];char octal[9]={0};std::sprintf(octal,"%06o",sum);std::memcpy(&header[148],octal,6);header[154]=0;header[155]=' ';
    tar.insert(tar.end(),header.begin(),header.end());tar.insert(tar.end(),data.begin(),data.end());tar.resize((tar.size()+511)/512*512,0);
}
struct TempArchive {
    std::vector<std::string> files;
    ~TempArchive(){for(size_t i=0;i<files.size();++i)DeleteFileA(files[i].c_str());}
    std::string write(Bytes data,bool archive=true){
        if(archive)data.resize(data.size()+1024,0);char folder[MAX_PATH+1]={0},name[MAX_PATH+1]={0};
        check(GetTempPathA(MAX_PATH,folder)!=0&&GetTempFileNameA(folder,"obc",0,name)!=0,"temporary TAR path");
        HANDLE file=CreateFileA(name,GENERIC_WRITE,0,0,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,0);check(file!=INVALID_HANDLE_VALUE,"open temporary TAR");
        DWORD actual=0;BOOL ok=WriteFile(file,data.empty()?0:&data[0],DWORD(data.size()),&actual,0);CloseHandle(file);files.push_back(name);check(ok&&actual==data.size(),"write temporary TAR/raw PIT");return name;
    }
};
struct RepartitionEvents {
    const EJODIN4_FlashResult* result;Device* device;bool cancelled;uint32_t cancel_phase;std::vector<uint32_t> phases;
    RepartitionEvents():result(0),device(0),cancelled(false),cancel_phase(0){}
};
int32_t EJODIN4_CALL repartition_cancel(void* context){return static_cast<RepartitionEvents*>(context)->cancelled?1:0;}
void EJODIN4_CALL repartition_progress(const EJODIN4_FlashProgress* event,void* context){
    RepartitionEvents& events=*static_cast<RepartitionEvents*>(context);events.phases.push_back(event->phase);
    const uint32_t flags=events.result->flags;
    if(flags&EJODIN4_FLASH_PIT_VERIFIED)check((flags&(EJODIN4_FLASH_PIT_UPLOADED|EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE))==(EJODIN4_FLASH_PIT_UPLOADED|EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE),"verified PIT always implies uploaded, started and possible write");
    if(flags&EJODIN4_FLASH_PIT_UPLOADED)check((flags&(EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE))==(EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE),"uploaded PIT always implies started and possible write");
    if(flags&EJODIN4_FLASH_PIT_WRITE_STARTED)check((flags&EJODIN4_FLASH_ANY_WRITE)!=0,"started PIT always implies possible write");
    if(event->phase==EJODIN4_PHASE_IMAGE_BEGIN){check((flags&EJODIN4_FLASH_PIT_VERIFIED)!=0,"no image begins before verified candidate readback");events.device->image_bytes=event->image_bytes;events.device->data_written=0;}
    if(event->phase==events.cancel_phase)events.cancelled=true;
}
void composed_repartition_tests(){
    const uint32_t proof=EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_PIT_UPLOADED|EJODIN4_FLASH_PIT_VERIFIED;
    for(unsigned network=0;network<2;++network){
    for(unsigned mode=0;mode<41;++mode){
        TempArchive temp;FakePlatform platform(mode==14);Bytes candidate(28+4*132,0);put(&candidate[0],0x12349876);put(&candidate[0]+4,4);
        const char* parts[]={"BOOT","SYSTEM","MODEM","CACHE"};const char* names[]={"boot.img","system.img","modem.bin","cache.img"};
        for(unsigned i=0;i<4;++i){uint8_t* entry=&candidate[0]+28+132*i;put(entry+4,2);put(entry+8,i+1);put(entry+24,mode==12||mode==27?1:16);std::strcpy(reinterpret_cast<char*>(entry+36),parts[i]);std::strcpy(reinterpret_cast<char*>(entry+68),names[i]);}
        if(mode==13||mode==28)put(&candidate[0]+28+132*3,1);
        std::string paths[5];EJODIN4_FirmwareFile files[5];
        const bool raw_target=mode==1||mode==2||(mode>=17&&mode<=25);
        for(unsigned i=0;i<4;++i){Bytes package;append_package(package,names[i],Bytes((mode==12||mode==27)&&i==1?513:1,0x5a));
            if(i==3){Bytes embedded=candidate;if(raw_target)put(&embedded[0]+28+12,0x55);append_package(package,"new-layout.pit",embedded);}
            paths[i]=temp.write(package);files[i].role=i+1;files[i].path=paths[i].c_str();files[i].path_bytes=uint32_t(paths[i].size()+1);}
        if(raw_target){paths[4]=temp.write(candidate,false);files[4].role=EJODIN4_PACKAGE_PIT_TARGET;files[4].path=paths[4].c_str();files[4].path_bytes=uint32_t(paths[4].size()+1);}
        EJODIN4_FlashRequest request=read_pit_request();request.operation=EJODIN4_FLASH_REPARTITION;request.files=files;request.file_count=raw_target?5:4;request.data_policy=EJODIN4_CLEAN_INSTALL;request.decoded_limit=1048576;request.logical_sector_bytes=mode==2?4096:512;
        if(mode==2)platform.d.pit_keep_old=true;
        if(mode==3){platform.d.pit_fault_stage=0;platform.d.pit_fault_mode=1;}
        if(mode==4){platform.d.pit_fault_stage=1;platform.d.pit_fault_mode=2;}
        if(mode==5)platform.d.pit_raw_limit=79;
        if(mode==6){platform.d.pit_fault_stage=2;platform.d.pit_fault_mode=2;}
        if(mode==7){platform.d.pit_fault_stage=3;platform.d.pit_fault_mode=3;}
        if(mode==8){platform.d.pit_fault_stage=6;platform.d.pit_fault_mode=1;}
        if(mode==15||mode==16)platform.d.protocol=mode==15?4:5;
        if(mode==17){request.operation=EJODIN4_FLASH_PLAN_ONLY;request.device_path=0;request.device_path_bytes=0;}
        if(mode==18)request.operation=EJODIN4_FLASH_WRITE;
        if(mode==19)request.operation=EJODIN4_FLASH_READ_PIT;
        if(mode==20)request.file_count=3;
        if(mode==21)request.data_policy=EJODIN4_PRESERVE_DATA;
        if(mode==22)files[3].role=EJODIN4_PACKAGE_HOME_CSC;
        if(mode==23)files[3].role=EJODIN4_PACKAGE_PIT_REFERENCE;
        if(mode==24)files[3].role=EJODIN4_PACKAGE_PIT_TARGET;
        if(mode==25)request.flags=2;
        if(mode>=26){request.operation=EJODIN4_FLASH_PLAN_ONLY;request.device_path=0;request.device_path_bytes=0;request.flags=EJODIN4_FLASH_PLAN_REPARTITION;}
        if(mode==29)request.flags|=EJODIN4_FLASH_REBOOT;
        if(mode>=30&&mode<=32){request.operation=mode==30?EJODIN4_FLASH_WRITE:mode==31?EJODIN4_FLASH_READ_PIT:EJODIN4_FLASH_REPARTITION;request.device_path="bound-interface";request.device_path_bytes=16;}
        if(mode==33)request.data_policy=EJODIN4_PRESERVE_DATA;
        if(mode==34)request.data_policy=EJODIN4_UPDATE_SELECTED;
        if(mode==35)request.file_count=3;
        if(mode==36)files[3].role=EJODIN4_PACKAGE_HOME_CSC;
        if(mode==37)files[3].role=EJODIN4_PACKAGE_PIT_REFERENCE;
        if(mode==38){request.device_path="bound-interface";request.device_path_bytes=16;}
        if(mode==39)request.flags|=8;
        if(mode==40)request.flags|=2;
        if(network&&mode!=14&&request.device_path){request.device_path="tcp-listen://127.0.0.1:13579";request.device_path_bytes=uint32_t(std::strlen(request.device_path)+1);}
        EJODIN4_FlashResult result;std::memset(&result,0,sizeof(result));char diagnostic[1024]={0};RepartitionEvents events;events.result=&result;events.device=&platform.d;
        events.cancel_phase=mode==9?EJODIN4_PHASE_PIT_UPLOAD_BEGIN:mode==10?EJODIN4_PHASE_PIT_UPLOAD_COMPLETE:mode==11?EJODIN4_PHASE_PIT_VERIFIED:0;
        const EJODIN4_Status status=flash_with_platform(platform,&request,&result,repartition_progress,repartition_cancel,&events,diagnostic,sizeof(diagnostic));
        if(mode==0||mode==1){if(status!=EJODIN4_OK)std::fprintf(stderr,"repartition mode=%u status=%u diagnostic=%s\n",mode,unsigned(status),diagnostic);
            check(status==EJODIN4_OK&&result.phase==EJODIN4_PHASE_COMPLETE,"full CLEAN repartition completes from embedded or explicit raw candidate");
            check(result.transport==(network?EJODIN4_DOWNLOAD_TCP:EJODIN4_DOWNLOAD_SAMSUNG_SERIAL)&&platform.tcp_opens==(network?1u:0u)&&platform.enumerations==(network?0u:1u),"composed repartition preserves transport provenance and TCP bypasses USB discovery");
            check(!platform.d.zero_calls&&platform.d.closes==1&&platform.d.incoming.empty(),"composed native repartition has no USB boundaries and closes cleanly");
            check(platform.d.uploaded_pit==candidate&&platform.d.pit_starts==1&&platform.d.pit_uploads==1&&(result.flags&proof)==proof,"successful operation exposes all independent PIT proofs");
            check(result.images_completed==4&&result.image_count==4&&result.total_bytes==4&&result.acknowledged_bytes==4&&platform.d.images==4,"repartition firmware accounting excludes raw PIT bytes");
            check(std::find(events.phases.begin(),events.phases.end(),EJODIN4_PHASE_PIT_UPLOAD_BEGIN)!=events.phases.end()&&std::find(events.phases.begin(),events.phases.end(),EJODIN4_PHASE_PIT_UPLOAD_COMPLETE)!=events.phases.end()&&std::find(events.phases.begin(),events.phases.end(),EJODIN4_PHASE_PIT_VERIFIED)!=events.phases.end(),"repartition reports upload begin, upload completion and verified readback phases");
        }else if(mode==17||mode==26){check(status==EJODIN4_OK&&result.image_count==4&&!platform.d.commands&&platform.d.expected==4&&!(result.flags&(proof|EJODIN4_FLASH_ANY_WRITE)),"CLEAN raw or explicitly marked embedded repartition plan validates candidate without protocol I/O or PIT side effects");}
        else if((mode>=18&&mode<=25)||mode>=29){check(status==EJODIN4_INVALID_ARGUMENT&&!platform.d.commands&&!platform.d.pit_starts,"target operation, offline marker, full CLEAN roles, duplicate target and removed flag constraints reject before I/O");}
        else {
            if(status==EJODIN4_OK)std::fprintf(stderr,"unexpected repartition success mode=%u\n",mode);
            check(status!=EJODIN4_OK&&!platform.d.images&&!platform.d.finishes&&!result.images_completed&&!result.acknowledged_bytes,"repartition failure emits no firmware data, finalize or reboot");
            if(mode==9||mode==12||mode==13||mode==14||mode==15||mode==16||mode==27||mode==28){check(!(result.flags&(proof|EJODIN4_FLASH_ANY_WRITE))&&!platform.d.pit_starts,"preupload rejection or cancellation has no PIT side effect");}
            else check((result.flags&(EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE))==(EJODIN4_FLASH_PIT_WRITE_STARTED|EJODIN4_FLASH_ANY_WRITE),"poststart failure retains possible PIT mutation even with zero images");
            if(mode==2||mode==8||mode==10||mode==11)check((result.flags&EJODIN4_FLASH_PIT_UPLOADED)!=0,"readback and later cancellation retain final upload ACK proof");
            if(mode==2)check(status==EJODIN4_PIT_MAPPING_ERROR&&!(result.flags&EJODIN4_FLASH_PIT_VERIFIED),"explicit allocation units cannot bypass mismatched candidate readback");
            if(mode==12||mode==13||mode==14||mode==27||mode==28)check(!platform.d.commands&&platform.d.expected==4,"candidate capacity, unsupported later route and WinUSB reject before protocol commands");
            if(mode==27)check(status==EJODIN4_PIT_MAPPING_ERROR,"embedded repartition offline marker enforces candidate image capacity");
            if(mode==28)check(status==EJODIN4_DRIVER_UNSUPPORTED,"embedded repartition offline marker enforces every stock route before connecting");
        }
    }
    }
}

void composed_route_guard_tests(){
    TempArchive temp;FakePlatform platform;
    platform.d.pit.resize(28+2*132,0);put(&platform.d.pit[0]+4,2);put(&platform.d.pit[0]+28+4,2);
    uint8_t* later=&platform.d.pit[0]+28+132;
    // Keep valid block addressing so the fixture reaches the later binary-route guard.
    put(later,1);put(later+4,2);put(later+8,2);put(later+20,0x100000);put(later+24,16384);
    std::memcpy(later+36,"VBMETA",6);std::memcpy(later+68,"vbmeta.img",10);
    Bytes ap,reference;append_package(ap,"boot.img",Bytes(1,0x5a));append_package(ap,"vbmeta.img",Bytes(1,0x5a));append_package(reference,"layout.pit",platform.d.pit);
    const std::string ap_path=temp.write(ap),reference_path=temp.write(reference);
    EJODIN4_FirmwareFile files[2];files[0].role=EJODIN4_PACKAGE_AP;files[0].path=ap_path.c_str();files[0].path_bytes=uint32_t(ap_path.size()+1);
    files[1].role=EJODIN4_PACKAGE_PIT_REFERENCE;files[1].path=reference_path.c_str();files[1].path_bytes=uint32_t(reference_path.size()+1);
    Cancellation cancel={0,0};std::unique_ptr<FirmwareSet> planned;std::string error;
    check(prepare_firmware_set(files,2,EJODIN4_UPDATE_SELECTED,1048576,cancel,planned,error)==EJODIN4_OK,"compose real held archives for late unsupported route");
    check(planned->images.size()==2 && planned->images[0].route.binary_type==0 && planned->images[0].route.device_type==2 && planned->images[1].route.binary_type==1 && planned->images[1].route.device_type==2,"composition fixture has first reviewed route then unsupported later image");planned.reset();
    EJODIN4_FlashRequest request=read_pit_request();request.operation=EJODIN4_FLASH_WRITE;request.files=files;request.file_count=2;request.data_policy=EJODIN4_UPDATE_SELECTED;request.decoded_limit=1048576;request.logical_sector_bytes=512;
    EJODIN4_FlashResult result;std::memset(&result,0,sizeof(result));char diagnostic[1024]={0};
    const EJODIN4_Status write_status=flash_with_platform(platform,&request,&result,0,0,0,diagnostic,sizeof(diagnostic));
    if(write_status!=EJODIN4_DRIVER_UNSUPPORTED)std::fprintf(stderr,"composed route guard: status=%u phase=%u flags=%u diagnostic=%s\n",unsigned(write_status),unsigned(result.phase),unsigned(result.flags),diagnostic);
    check(write_status==EJODIN4_DRIVER_UNSUPPORTED,"composed WRITE rejects later unsupported route");
    check(platform.d.total==2 && !platform.d.images && !platform.d.sequences && !platform.d.finishes && !(result.flags&EJODIN4_FLASH_ANY_WRITE),"all routes validated before even the first reviewed image changes flash");
    check(!result.images_completed && !result.acknowledged_bytes && std::strstr(diagnostic,"route"),"late unsupported route failure retains zero-write diagnostic evidence");
    FakePlatform discovery;discovery.d.pit=platform.d.pit;request.operation=EJODIN4_FLASH_READ_PIT;
    check(flash_with_platform(discovery,&request,&result,0,0,0,diagnostic,sizeof(diagnostic))==EJODIN4_OK,"READ_PIT may inspect prepared firmware without dispatching images");
    check(result.total_bytes==2 && result.image_count==2 && !discovery.d.total && !discovery.d.images && !(result.flags&EJODIN4_FLASH_ANY_WRITE),"READ_PIT prepared plan never announces live write totals");
    for(size_t k=0;k<discovery.d.transcript.size();++k)check(!(discovery.d.transcript[k][0]==0x64 && discovery.d.transcript[k][1]==2),"READ_PIT has no total notification command");
}
struct CapacityEvents {
    const EJODIN4_FlashResult* result;Device* device;bool expected_mixed,validated,seen_image;uint32_t last_image;
    CapacityEvents():result(0),device(0),expected_mixed(false),validated(false),seen_image(false),last_image(0){}
};
void EJODIN4_CALL capacity_progress(const EJODIN4_FlashProgress* event,void* context){
    CapacityEvents& state=*static_cast<CapacityEvents*>(context);
    if(event->phase==EJODIN4_PHASE_PLAN_VALIDATED){state.validated=true;check(state.result!=0&&((state.result->flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS)!=0)==state.expected_mixed,"mixed-unit result marker precedes PLAN_VALIDATED callback");}
    if(event->phase==EJODIN4_PHASE_IMAGE_BEGIN&&state.device&&(!state.seen_image||state.last_image!=event->image_index)){
        state.device->image_bytes=event->image_bytes;state.device->data_written=0;state.last_image=event->image_index;state.seen_image=true;
    }
}
void generic_storage_operation_tests(){
    for(unsigned usb=0;usb<2;++usb)for(unsigned mode=0;mode<18;++mode){
        const bool mixed=mode>=14;const bool mmc=mode==1||mode==2||mode==5;
        const uint32_t payload_bytes=mode==1?512u:mode==2?513u:mode==3?4097u:2048u;
        TempArchive temp;FakePlatform platform(usb!=0);put(&platform.d.pit[0]+28+4,mmc?2:8);put(&platform.d.pit[0]+28+24,mode==11?0:1);
        std::memset(&platform.d.pit[0]+28+36,0,64);std::strcpy(reinterpret_cast<char*>(&platform.d.pit[0]+28+36),"VENDOR_BOOT");std::strcpy(reinterpret_cast<char*>(&platform.d.pit[0]+28+68),"vendor_boot.img");
        Bytes ap,reference;
        if(mixed){platform.d.pit.resize(28+2*132,0);put(&platform.d.pit[0]+4,2);uint8_t* first=&platform.d.pit[0]+28;put(first+4,2);std::memset(first+36,0,64);std::strcpy(reinterpret_cast<char*>(first+36),"BOOT");std::strcpy(reinterpret_cast<char*>(first+68),"boot.img");uint8_t* second=first+132;put(second+4,mode==17?1:8);put(second+8,2);put(second+24,1);std::strcpy(reinterpret_cast<char*>(second+36),"VENDOR_BOOT");std::strcpy(reinterpret_cast<char*>(second+68),"vendor_boot.img");append_package(ap,"boot.img",Bytes(mode==16?513:512,0x5a));}
        append_package(ap,"vendor_boot.img",Bytes(payload_bytes,0x5a));append_package(reference,"arbitrary_other_model.pit",platform.d.pit);
        const std::string ap_path=temp.write(ap),reference_path=temp.write(reference);EJODIN4_FirmwareFile files[2];files[0].role=EJODIN4_PACKAGE_AP;files[0].path=ap_path.c_str();files[0].path_bytes=uint32_t(ap_path.size()+1);files[1].role=EJODIN4_PACKAGE_PIT_REFERENCE;files[1].path=reference_path.c_str();files[1].path_bytes=uint32_t(reference_path.size()+1);
        EJODIN4_FlashRequest request=read_pit_request();request.operation=EJODIN4_FLASH_WRITE;request.files=files;request.file_count=2;request.data_policy=EJODIN4_UPDATE_SELECTED;request.decoded_limit=1048576;
        if(mode==4)request.logical_sector_bytes=512;if(mode==5)request.logical_sector_bytes=4096;
        if(mode==6)platform.d.pit[8]^=1;
        if(mode==7||mode==8||mode==9)request.flags=2; // Removed request bit must remain rejected.
        if(mode==8||mode==12||mode==15)request.operation=EJODIN4_FLASH_READ_PIT;
        if(mode==9||mode==13){request.operation=EJODIN4_FLASH_PLAN_ONLY;request.device_path=0;request.device_path_bytes=0;}
        if(mode==10)request.logical_sector_bytes=8192;
        EJODIN4_FlashResult result;std::memset(&result,0,sizeof(result));char diagnostic[1024]={0};CapacityEvents events;events.result=&result;events.device=&platform.d;events.expected_mixed=mode==14;
        const EJODIN4_Status status=flash_with_platform(platform,&request,&result,capacity_progress,0,&events,diagnostic,sizeof(diagnostic));
        const bool success=mode==0||mode==1||mode==5||mode==12||mode==13||mode==14||mode==15;
        const bool invalid=mode==7||mode==8||mode==9||mode==10;
        if(success&&status!=EJODIN4_OK)std::fprintf(stderr,"automatic storage operation usb=%u mode=%u status=%u: %s\n",usb,mode,unsigned(status),diagnostic);
        check(status==(success?EJODIN4_OK:invalid?EJODIN4_INVALID_ARGUMENT:EJODIN4_PIT_MAPPING_ERROR),"automatic PIT units enforce image capacity and exact package/live identity");
        if(success&&request.operation==EJODIN4_FLASH_WRITE){const uint64_t total=payload_bytes+(mixed?512u:0u);check(result.images_completed==(mixed?2u:1u)&&result.acknowledged_bytes==total&&platform.d.total==total&&events.validated,"generic arbitrary build/hash plan completes fake writes using storage-type units");check(result.logical_sector_bytes==(mixed?0u:mmc?(request.logical_sector_bytes?request.logical_sector_bytes:512u):4096u),"homogeneous plan reports common unit; mixed plan reports zero common unit");check(((result.flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS)!=0)==mixed,"only mixed selected units need per-partition result marker");}
        else{check(!platform.d.images&&!platform.d.sequences&&!result.acknowledged_bytes&&!(result.flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS),"read-only or rejected plan cannot write or claim mixed write provenance");if(invalid||mode==13)check(!platform.d.commands,"invalid options and offline plans perform no device commands");if(!success)check(!events.validated,"invalid capacity or layout never reaches PLAN_VALIDATED");}
        if(mode==15)check(result.logical_sector_bytes==0&&events.validated,"mixed read-only PIT validation has no write-only result flag");
    }
}
void composed_encoded_dialect_tests(){
 for(unsigned mode=0;mode<5;++mode){TempArchive temp;FakePlatform platform;platform.d.protocol=mode==0?1:mode==1?2:mode==2?3:mode==3?4:5;platform.d.packet=mode==0?131072:1048576;platform.d.begin_capability=mode!=2;put(&platform.d.pit[0]+28+4,2);
 Bytes payload(513,0x5a);LZ4F_preferences_t prefs;std::memset(&prefs,0,sizeof(prefs));prefs.frameInfo.blockSizeID=LZ4F_max64KB;prefs.frameInfo.blockMode=LZ4F_blockIndependent;prefs.frameInfo.contentSize=payload.size();Bytes frame(LZ4F_compressFrameBound(payload.size(),&prefs));size_t size=LZ4F_compressFrame(&frame[0],frame.size(),&payload[0],payload.size(),&prefs);check(!LZ4F_isError(size),"compose eligible native encoded image");frame.resize(size);Bytes package;append_package(package,"boot.img.lz4",frame);append_package(package,"layout.pit",platform.d.pit);std::string p=temp.write(package);EJODIN4_FirmwareFile file={EJODIN4_PACKAGE_AP,p.c_str(),uint32_t(p.size()+1)};
 EJODIN4_FlashRequest request=read_pit_request();request.operation=EJODIN4_FLASH_WRITE;request.files=&file;request.file_count=1;request.data_policy=EJODIN4_UPDATE_SELECTED;request.decoded_limit=1048576;EJODIN4_FlashResult result;std::memset(&result,0,sizeof(result));char diagnostic[1024]={0};CapacityEvents events;events.result=&result;events.device=&platform.d;
 EJODIN4_Status status=flash_with_platform(platform,&request,&result,capacity_progress,0,&events,diagnostic,sizeof(diagnostic));
 if(mode<3){check(status==EJODIN4_OK&&result.images_completed==1&&result.acknowledged_bytes==513&&(result.flags&EJODIN4_FLASH_FINALIZED),"eligible compressed archive uses decoded raw wire on older or capability-free dialect");check(platform.d.encoded_completions.empty(),"compressed eligibility does not force unavailable wire dialect");}
 else{check(status==EJODIN4_DRIVER_UNSUPPORTED&&std::strstr(diagnostic,"signed")&&!platform.d.images&&!result.acknowledged_bytes&&!(result.flags&EJODIN4_FLASH_ANY_WRITE),"newer composed WRITE rejects missing signed identity before all image writes");check(platform.d.transcript.size()==1&&platform.d.transcript[0][0]==0x64,"missing identity rejects immediately after negotiation before DEVINFO or PIT");}
 }
}

void automatic_userdata_capacity_tests(){
    for(unsigned usb=0;usb<2;++usb)for(unsigned mode=0;mode<3;++mode){
        TempArchive temp;FakePlatform platform(usb!=0);platform.d.pit.assign(28+4*132,0);put(&platform.d.pit[0],0x12349876);put(&platform.d.pit[0]+4,4);
        const char* names[]={"boot.img","userdata.img","modem.bin","cache.img"};const char* parts[]={"BOOT","USERDATA","RADIO","CACHE"};
        for(unsigned i=0;i<4;++i){uint8_t* entry=&platform.d.pit[0]+28+i*132;put(entry+4,8);put(entry+8,i+1);put(entry+24,i==1?0:16);std::strcpy(reinterpret_cast<char*>(entry+36),parts[i]);std::strcpy(reinterpret_cast<char*>(entry+68),names[i]);}
        std::string paths[4];EJODIN4_FirmwareFile files[4];for(unsigned i=0;i<4;++i){Bytes package;append_package(package,names[i],Bytes(i==1?8192:1,0x5a));if(i==3)append_package(package,"generic_layout.pit",platform.d.pit);paths[i]=temp.write(package);files[i].role=i+1;files[i].path=paths[i].c_str();files[i].path_bytes=uint32_t(paths[i].size()+1);}
        EJODIN4_FlashRequest request=read_pit_request();request.operation=EJODIN4_FLASH_WRITE;request.files=files;request.file_count=4;request.data_policy=EJODIN4_CLEAN_INSTALL;request.decoded_limit=1048576;request.userdata_capacity_bytes=mode==1?8192:mode==2?4096:0;
        EJODIN4_FlashResult result;std::memset(&result,0,sizeof(result));char diagnostic[1024]={0};CapacityEvents events;events.result=&result;events.device=&platform.d;
        const EJODIN4_Status status=flash_with_platform(platform,&request,&result,capacity_progress,0,&events,diagnostic,sizeof(diagnostic));
        if(mode==1&&status!=EJODIN4_OK)std::fprintf(stderr,"auto USERDATA capacity usb=%u: %s\n",usb,diagnostic);
        check(status==(mode==1?EJODIN4_OK:EJODIN4_PIT_MAPPING_ERROR),"zero-count USERDATA requires independent sufficient capacity even with auto units");
        if(mode==1)check(result.images_completed==4&&result.acknowledged_bytes==8195&&result.logical_sector_bytes==4096&&!(result.flags&EJODIN4_FLASH_PER_PARTITION_PIT_UNITS),"verified USERDATA capacity with unit0 request is honored using UFS4096 allocation units");
        else check(!platform.d.images&&!platform.d.sequences&&!result.acknowledged_bytes,"unknown or undersized USERDATA capacity blocks every earlier image");
    }
}
void integrated_operation_tests(){
    for(unsigned mode=0;mode<8;++mode){
        FakePlatform platform;OperationEvents events;EJODIN4_FlashRequest request=read_pit_request();EJODIN4_FlashResult result;std::memset(&result,0xa5,sizeof(result));char diagnostic[512]={0};
        if(mode==1)events.cancelled=true;
        if(mode==2)platform.d.handshake_bad=true;
        if(mode==3)platform.d.fail_command=0x67;
        if(mode==4)events.throw_phase=EJODIN4_PHASE_PREFLIGHT;
        if(mode==5)events.throw_phase=EJODIN4_PHASE_PIT_ENTRY;
        if(mode==6)events.cancel_phase=EJODIN4_PHASE_PIT_READY;
        if(mode==7)platform.d.fail_close=true;
        const EJODIN4_Status status=flash_with_platform(platform,&request,&result,operation_progress,operation_cancel,&events,diagnostic,sizeof(diagnostic));
        check(result.struct_size==sizeof(result)&&result.operation==EJODIN4_FLASH_READ_PIT,"Integrated read PIT returns initialized native result");
        check(!result.images_completed&&!result.image_count&&!result.total_bytes&&!result.acknowledged_bytes,"PIT-only operation never claims image or byte transfers");
        check(!(result.flags&(EJODIN4_FLASH_ANY_WRITE|EJODIN4_FLASH_FINALIZED|EJODIN4_FLASH_REBOOT_SENT))&&!platform.d.images,"PIT-only operation never claims firmware writes, write finalization or reboot");
        if(mode==0){check(status==EJODIN4_OK&&result.phase==EJODIN4_PHASE_COMPLETE&&result.pit_entry_count==1&&result.protocol_version==2,"Integrated full PIT operation succeeds");check(events.pit_events==1&&events.complete_events==1&&platform.d.closes==1&&platform.d.finishes==1,"Integrated operation proves PIT event, checked finish, one cleanup and one completion");}
        else{check(status!=EJODIN4_OK&&events.complete_events==0,"Failure cannot emit successful completion");check(diagnostic[0]!=0,"Integrated failure carries diagnostic");}
        if(mode==1||mode==6)check(status==EJODIN4_CANCELLED&&result.phase==EJODIN4_PHASE_CANCELLED,"Integrated cancellation propagates native phase and status");
        if(mode==1||mode==4)check(!platform.d.commands&&!platform.d.closes,"Preflight cancellation or exception opens no connection");
        if(mode==4||mode==5)check(status==EJODIN4_INTERNAL_ERROR&&result.phase==EJODIN4_PHASE_FAILED,"Throwing progress is contained at C API boundary");
        if(mode==2||mode==5||mode==6)check(platform.d.closes==1&&!platform.d.finishes,"Interrupted PIT session cleans owned serial connection without finalizing");
        if(mode==3)check(status==EJODIN4_INVALID_RESPONSE&&platform.d.finishes==1,"Integrated final negative ACK rejects completion");
        if(mode==7)check(status==EJODIN4_USB_IO_ERROR&&result.phase==EJODIN4_PHASE_FAILED,"Integrated native cleanup failure rejects success");
    }
    {FakePlatform platform;OperationEvents events;EJODIN4_FlashRequest request=read_pit_request();request.flags=EJODIN4_FLASH_REBOOT;EJODIN4_FlashResult result;std::memset(&result,0xa5,sizeof(result));const EJODIN4_FlashResult original=result;char diagnostic[256]={0};check(flash_with_platform(platform,&request,&result,operation_progress,operation_cancel,&events,diagnostic,sizeof(diagnostic))==EJODIN4_INVALID_ARGUMENT,"PIT-only operation cannot request a reboot");check(!std::memcmp(&original,&result,sizeof(result))&&events.phases.empty()&&!platform.d.commands,"Invalid request preserves result bytes and emits no I/O or progress");}
}

}
int main() {
    try {
        tcp_session_tests();repartition_session_tests();composed_repartition_tests();native_devinfo_versions();stock_encoded_tests();stock_raw_golden();handshake_diagnostics();stock_versions_and_routes();
        happy(513,true,false,false,false);
        happy(513,false,false,true,true);
        happy(30u*1024u*1024u+513u,true,false,false,true);
        happy(513,true,true,false,false);
        happy(513,true,false,false,false,true);
        happy(513,true,true,false,false,true);
        happy(uint64_t(0x100000000ull)+513,true,false,false,true);
        delayed_data_ack_tests();failures();integrated_operation_tests();composed_route_guard_tests();generic_storage_operation_tests();composed_encoded_dialect_tests();automatic_userdata_capacity_tests();
        std::printf("Native BCC session fake tests passed (%u checks)\n",checks);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL after %u checks: %s\n",checks,e.what());return 1;}
}
