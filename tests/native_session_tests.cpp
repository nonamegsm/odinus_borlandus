#include "Odin4Session.h"
#include "Odin4FlashOperation.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
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
    bool fail_reset;
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
    std::vector<uint8_t> pit,write_buffer;
    std::deque<uint8_t> incoming;
    std::vector<std::vector<uint32_t> > transcript;
    Device() : clock(0),total(0),image_bytes(0),data_written(0),protocol(2),packet(1048576),expected(4),packet_index(0),packets(0),commands(0),images(0),sequences(0),finishes(0),closes(0),zero_calls(0),wrong_command(0),fail_command(0),wrong_index(UINT32_MAX),fail_reset(false),optional_mode(0),read_zeroes(0),zero_advance(0),metadata_tail_bytes(0),change_metadata_framing(false),cancelled(false),fragment(true),fail_close(false),fail_write(false),handshake_bad(false),begin_capability(true),zero_cancel(false),pit_short(false),completion_fail(false),pit_size_override(0),data_ack_delay_ms(0),data_ack_ready_at(0),data_ack_budget_ms(0),data_write_budget_ms(0),awaiting_data_ack(false),pit(28+132,0) {
        put((&pit[0]),0x12349876);put((&pit[0])+4,1);
        put((&pit[0])+28+8,1); // identifier
        put((&pit[0])+28+24,0x100000);
        std::memcpy((&pit[0])+28+36,"BOOT",4);
        std::memcpy((&pit[0])+28+68,"boot.img",8);
    }
    void queue(const uint8_t* p,size_t n) { incoming.insert(incoming.end(),p,p+n); }
    void ack(uint32_t id,uint32_t value=0) {
        uint8_t b[8]={0};put(b,id);put(b+4,value);queue(b,8);
    }
    void received() {
        if(expected==4) {
            check(!std::memcmp((&write_buffer[0]),"ODIN",4),"four byte handshake");
            queue(reinterpret_cast<const uint8_t*>(handshake_bad?"NOPE":"LOKE"),4);
            expected=1024;
        } else if(packets) {
            check(write_buffer.front()==0x5a,"literal image byte");
            const uint64_t remaining=image_bytes-data_written;
            const uint32_t genuine=uint32_t(std::min<uint64_t>(remaining,packet));
            if(genuine<packet) check(write_buffer[genuine]==0 && write_buffer.back()==0,"last packet zero padding");
            data_written+=genuine;
            ack(0,wrong_index==packet_index?packet_index+1:packet_index);
            awaiting_data_ack=true;data_ack_ready_at=clock+data_ack_delay_ms;
            ++packet_index;--packets;expected=packets?packet:1024;
        } else {
            check(write_buffer.size()==1024,"control size");
            const uint32_t cmd=get((&write_buffer[0])),sub=get((&write_buffer[0])+4);
            std::vector<uint32_t> request;
            for(unsigned i=0;i<10;++i) request.push_back(get((&write_buffer[0])+4*i));
            transcript.push_back(request);++commands;
            for(size_t i=40;i<write_buffer.size();++i) check(write_buffer[i]==0,"control zero tail");
            if(cmd==0x64 && sub==0) check(request[2]==0x7fffffffu,"maximum protocol negotiation payload");
            if(cmd==0x64 && sub==5) check(request[2]==1048576,"modern packet size negotiation");
            if(cmd==0x66 && sub==2) check(request[2]>0 && request[2]<=30u*1024u*1024u && request[2]%packet==0,"bounded aligned sequence size");
            if(cmd==0x65 && sub==2) {
                const uint32_t offset=get((&write_buffer[0])+8)*500;
                check(offset<pit.size(),"PIT block index");
                const size_t n=std::min<size_t>(500,pit.size()-offset);
                queue((&pit[0])+offset,pit_short?n-1:n);
                if(offset+n==pit.size()) {
                    if(optional_mode==1) ack(0x65);
                    else if(optional_mode==2) { uint8_t partial[3]={0x65,0,0};queue(partial,3); }
                    else if(optional_mode==3) ack(0x66);
                    else if(optional_mode==4) ack(0x65,0xfffffffeu);
                }
            } else {
                uint32_t value=0;
                if(cmd==0x64 && sub==0) value=(protocol<<16)|(begin_capability?0x80000000u:0);
                if(cmd==0x65 && sub==1) value=pit_size_override?pit_size_override:uint32_t(pit.size());
                if(cmd==0x64 && sub==2) total=uint64_t(get((&write_buffer[0])+8))|(uint64_t(get((&write_buffer[0])+12))<<32);
                if(cmd==0x66 && sub==0) ++images;
                if(cmd==0x66 && sub==2) { ++sequences;packets=get((&write_buffer[0])+8)/packet;packet_index=0;expected=packet; }
                if(cmd==0x66 && sub==3 && completion_fail) value=0xfffffffeu;
                if(cmd==0x64 && sub==1 && fail_reset) value=0xfffffffeu;
                if(cmd==0x67 && sub==0) ++finishes;
                if(cmd==fail_command) value=0xfffffffeu;
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
            for(uint32_t i=0;i<actual;++i){p[i]=incoming.front();incoming.pop_front();}
            if(incoming.empty())awaiting_data_ack=false;
        } else {
            if(write_buffer.empty()){
                if(expected==packet)data_write_budget_ms=until-clock;
                else control_budgets.push_back(until-clock);
            }
            if(fail_write && expected==packet) {error="injected uncertain write";return EJODIN4_USB_IO_ERROR;}
            actual=std::min<uint32_t>(n,expected-uint32_t(write_buffer.size()));
            if(fragment && expected<=1024) actual=std::min<uint32_t>(actual,17);
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
    bool duplicate,missing;
    uint64_t open_budget_ms;
    explicit FakePlatform(bool usb=false):duplicate(false),missing(false),open_budget_ms(0) {std::memset(&identity,0,sizeof(identity));identity.vendor_id=0x04e8;identity.product_id=0x685d;identity.flags=usb?1:2;std::memcpy(identity.device_path,"bound-interface",16);}
    uint64_t now() const {return d.clock;}
    EJODIN4_Status enumerate(std::vector<EJODIN4_UsbDevice>& v,const Cancellation&,std::string&) {if(!missing)v.push_back(identity);if(duplicate)v.push_back(identity);return EJODIN4_OK;}
    EJODIN4_Status open(const EJODIN4_UsbDevice&,std::unique_ptr<Connection>& p,uint64_t,const Cancellation&,std::string&) {p.reset(new Usb(d));return EJODIN4_OK;}
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
struct Fixture {
    FakePlatform p;
    DownloadChannel c;
    Session s;
    std::string error;
    std::vector<EJODIN4_PitEntry> pit;
    explicit Fixture(bool usb=false,uint32_t timeout_override=0):p(usb),c(p,make_cancel(&p.d)),s(c,SessionProgress(),timeout_override){}
    EJODIN4_Status begin(){EJODIN4_Status r=c.open("bound-interface",16,30000,error);return r==EJODIN4_OK?s.begin(pit,error):r;}
    PlanImage image(uint64_t n,bool large=false) {PlanImage i;i.decoded_bytes=n;i.route.identifier=1;i.route.device_type=2;i.large_partition=large;p.d.image_bytes=n;return i;}
};
void happy(uint64_t bytes,bool modern,bool usb,bool extra,bool large,bool metadata_tail=false) {
    Fixture f(usb);f.p.d.protocol=modern?2:1;f.p.d.packet=modern?1048576:131072;f.p.d.optional_mode=extra?1:0;
    f.p.d.metadata_tail_bytes=metadata_tail?4:0;
    check(f.begin()==EJODIN4_OK,"begin session");check(f.pit.size()==1,"live PIT parsed");
    check(f.s.pit_bytes()==f.p.d.pit,"exact valid raw PIT retained");
    check(f.s.state().protocol_version==(modern?2u:1u),"unsigned compression bit masked");
    check(f.s.state().metadata_tail_present==metadata_tail,"metadata response framing established before images");
    if(metadata_tail)check(f.s.state().metadata_tail_word==0x12345678u,"opaque device word retained");
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
        if(row[7])check(row[3]==(large?uint32_t((bytes%(30u*1024u*1024u)+511)/512*512):uint32_t(bytes%(30u*1024u*1024u))),"last sequence real size");
    }}
    for(size_t rowIndex=0;rowIndex<f.p.d.transcript.size();++rowIndex) {const std::vector<uint32_t>& row=f.p.d.transcript[rowIndex];if(row[0]==0x64 && row[1]==1){++resets;check(row[2]==0,"empty reset counter payload");}}
    check(resets==2,"one initial query plus one reset after completed image");
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
    for(unsigned size=1;size<4;++size){Fixture f;f.p.d.metadata_tail_bytes=size;check(f.begin()!=EJODIN4_OK && !f.s.state().any_write,"partial metadata frame blocks images");}
    {Fixture f;f.p.d.pit.resize(160+500,0);f.p.d.pit.back()=0x93;check(f.begin()==EJODIN4_OK,"PIT multi block final short raw bytes");check(f.s.pit_bytes()==f.p.d.pit,"raw PIT signature and trailing bytes retained exactly");check(f.s.finish(false,f.error)==EJODIN4_OK,"discovery close without image or reboot");check(!f.s.state().any_write && !f.s.state().reboot_sent,"discovery truthful");}
    {Fixture f;f.p.d.pit.resize(1000,0);check(f.begin()==EJODIN4_OK,"PIT exact 500 byte block multiple");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin modem binary route");f.s.notify_total(10,f.error);PlanImage i=f.image(10);i.route.binary_type=1;i.route.identifier=99;Reader r(10);check(f.s.send_image(i,r,f.error)==EJODIN4_OK,"modem route transfer");for(size_t rowIndex=0;rowIndex<f.p.d.transcript.size();++rowIndex) {const std::vector<uint32_t>& row=f.p.d.transcript[rowIndex];if(row[0]==0x66 && row[1]==3)check(row[2]==1 && row[4]==1 && row[6]==0,"modem destination omits partition identifier");}}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin wrong index case");check(f.s.notify_total(10,f.error)==EJODIN4_OK,"set total");f.p.d.wrong_index=0;PlanImage i=f.image(10);Reader r(10);check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"wrong packet index");check(f.s.state().acknowledged_bytes==0 && f.s.state().any_write,"uncertain write truthful");const uint32_t n=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK && f.p.d.commands==n,"no commands after error");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin completion failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.completion_fail=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"negative -2 not progress");check(f.s.state().acknowledged_bytes==10 && !f.s.state().images_completed,"packet ACK does not mean image complete");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin reset failure");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.fail_reset=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"reset counter strict ACK");check(f.s.state().images_completed==1 && f.s.state().acknowledged_bytes==10,"written image retained after reset failure");const uint32_t n=f.p.d.commands;check(f.s.finish(true,f.error)!=EJODIN4_OK && f.p.d.commands==n,"reset failure stops reboot and next commands");}
    {Fixture f;check(f.begin()==EJODIN4_OK,"begin framing change");f.s.notify_total(10,f.error);PlanImage i=f.image(10);Reader r(10);f.p.d.change_metadata_framing=true;check(f.s.send_image(i,r,f.error)==EJODIN4_INVALID_RESPONSE,"changed reset framing fails");check(f.s.state().images_completed==1 && !f.s.state().reboot_sent,"framing error retains completed image");}
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
        happy(513,true,false,false,false);
        happy(513,false,false,true,true);
        happy(30u*1024u*1024u+513u,true,false,false,true);
        happy(513,true,true,false,false);
        happy(513,true,false,false,false,true);
        happy(513,true,true,false,false,true);
        happy(uint64_t(0x100000000ull)+513,true,false,false,true);
        delayed_data_ack_tests();failures();integrated_operation_tests();
        std::printf("Native BCC session fake tests passed (%u checks)\n",checks);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"FAIL after %u checks: %s\n",checks,e.what());return 1;}
}
