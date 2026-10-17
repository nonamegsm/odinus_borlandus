// Offline tests only: every USB/discovery/time/I/O call below uses fake seams.
#include "Odin4UsbTransport.h"
#include "Odin4SamsungSerial.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
int32_t EJODIN4_CALL cancel_yes(void*) { return 1; }
int32_t EJODIN4_CALL cancel_flag(void* flag) { return *static_cast<bool*>(flag) ? 1 : 0; }
const ejodin4::Cancellation never = {0, 0};
EJODIN4_UsbDevice device(const char* path = "fake-samsung-interface") {
    EJODIN4_UsbDevice result = {0};
    result.vendor_id = 0x04e8; result.product_id = 0x685d;
    result.flags = EJODIN4_USB_WINUSB_COMPATIBLE;
    const size_t path_size = std::strlen(path) + 1;
    check(path_size <= sizeof(result.device_path), "Fake path fits identity buffer");
    std::memcpy(result.device_path, path, path_size);
    const char identity[] = "USB\\VID_04E8&PID_685D\\fake";
    const char driver[] = "WinUSB";
    std::memcpy(result.instance_id, identity, sizeof(identity));
    std::memcpy(result.driver, driver, sizeof(driver));
    return result;
}
ejodin4::Interface bulk(uint8_t number = 2, uint8_t klass = 0x0a) {
    ejodin4::Interface i;i.number=number;i.device_class=klass;ejodin4::Pipe in={0x83,512,true},out={0x04,512,true};i.pipes.push_back(in);i.pipes.push_back(out);return i;
}
struct State {
    uint64_t clock;
    std::vector<EJODIN4_UsbDevice> devices;
    std::vector<ejodin4::Interface> interfaces;
    EJODIN4_Status enum_status, open_status;
    EJODIN4_Status interface_status,write_status,read_status;
    uint32_t write_count;
    std::vector<uint8_t> reply;
    std::vector<uint8_t> written;
    std::vector<std::string> events;
    bool cancel_after_write;
    bool* cancellation_flag;
    bool throw_enumerate;
    unsigned opens,enumerations,closes;
    uint32_t read_capacity;
    uint64_t enum_elapsed;
    State():clock(10),enum_status(EJODIN4_OK),open_status(EJODIN4_OK),interface_status(EJODIN4_OK),write_status(EJODIN4_OK),read_status(EJODIN4_OK),write_count(4),cancel_after_write(false),cancellation_flag(0),throw_enumerate(false),opens(0),enumerations(0),closes(0),read_capacity(0),enum_elapsed(0){devices.push_back(device());interfaces.push_back(bulk());const char bytes[]={'L','O','K','E'};reply.assign(bytes,bytes+4);}
};
class FakeConnection : public ejodin4::Connection {
    State& state_;
public:
    explicit FakeConnection(State& state) : state_(state) { state_.events.push_back("open"); }
    ~FakeConnection() { ++state_.closes; state_.events.push_back("close"); }
    EJODIN4_Status interfaces(std::vector<ejodin4::Interface>& result, std::string& error) {
        state_.events.push_back("interfaces"); result = state_.interfaces;
        if (state_.interface_status) error = "fake descriptor failure";
        return state_.interface_status;
    }
    EJODIN4_Status transfer(size_t index, uint8_t pipe, uint8_t* data, uint32_t capacity,
        uint32_t& actual, uint64_t deadline, const ejodin4::Cancellation& cancellation, std::string& error) {
        check(index < state_.interfaces.size(), "Selected interface belongs to current connection");
        check(deadline == 1010, "Transfers share one operation deadline");
        if (cancellation.requested()) return EJODIN4_CANCELLED;
        if (pipe & 0x80) {
            state_.events.push_back("read"); state_.read_capacity = capacity;
            check(pipe == 0x83, "Read uses selected bulk IN");
            actual = static_cast<uint32_t>(state_.reply.size());
            if (!state_.reply.empty()) std::memcpy(data, (&state_.reply[0]), std::min<size_t>(capacity, state_.reply.size()));
            if (state_.read_status) error = "fake read disconnect";
            return state_.read_status;
        }
        state_.events.push_back("write");
        check(pipe == 0x04, "Write uses selected bulk OUT");
        state_.written.assign(data, data + capacity); actual = state_.write_count;
        if (state_.cancel_after_write && state_.cancellation_flag) *state_.cancellation_flag = true;
        if (state_.write_status) error = "fake write disconnect";
        return state_.write_status;
    }
};
class FakePlatform : public ejodin4::Platform {
public:
    State state;
    uint64_t now() const { return state.clock; }
    EJODIN4_Status enumerate(std::vector<EJODIN4_UsbDevice>& devices,
        const ejodin4::Cancellation&, std::string& error) {
        ++state.enumerations; state.events.push_back("enumerate"); state.clock += state.enum_elapsed;
        if (state.throw_enumerate) throw std::runtime_error("fake allocation failure");
        devices = state.devices;
        if (state.enum_status) error = "fake enumeration error";
        return state.enum_status;
    }
    EJODIN4_Status open(const EJODIN4_UsbDevice& selected,
        std::unique_ptr<ejodin4::Connection>& result, uint64_t deadline,
        const ejodin4::Cancellation&, std::string& error) {
        ++state.opens;
        check(deadline == 1010, "Open shares the operation deadline");
        check(std::strcmp(selected.device_path, "fake-samsung-interface") == 0, "Open uses exact re-enumerated identity");
        if (state.open_status) { error = "fake open failure"; return state.open_status; }
        result.reset(new FakeConnection(state)); return EJODIN4_OK;
    }
};
EJODIN4_Status run(FakePlatform& p, EJODIN4_UsbProbe& output, const ejodin4::Cancellation& c = never) {
    std::string error;
    const char path[] = "fake-samsung-interface";
    return ejodin4::probe(p, path, sizeof(path), 1000, &output, c, error);
}
void failure(FakePlatform& p, EJODIN4_Status expected, const ejodin4::Cancellation& c = never) {
    EJODIN4_UsbProbe output;
    std::memset(&output, 0xa5, sizeof(output));
    const EJODIN4_UsbProbe original = output;
    check(run(p, output, c) == expected, "Expected probe failure");
    check(std::memcmp(&original, &output, sizeof(output)) == 0, "Failure preserves every output byte");
    check(p.state.opens == 0 || p.state.open_status || p.state.closes == 1, "Failure closes owned connection exactly once");
}
ejodin4::Cancellation cancel_value(EJODIN4_CancelCallback f,void* p){ejodin4::Cancellation c={f,p};return c;}
ejodin4::Pipe extra_in(){ejodin4::Pipe p={0x85,512,true};return p;}
std::vector<uint8_t> byte_vector(const char* p,size_t n){return std::vector<uint8_t>(p,p+n);}
std::vector<std::string> strings(const char* a,const char* b=0,const char* c=0,const char* d=0,const char* e=0,const char* f=0){const char* values[]={a,b,c,d,e,f};std::vector<std::string> result;for(unsigned i=0;i<6&&values[i];++i)result.push_back(values[i]);return result;}
void test_probe() {
    EJODIN4_UsbProbe output = {0};
    FakePlatform p;
    check(run(p, output) == EJODIN4_OK, "LOKE handshake success");
    check(p.state.enumerations == 1 && p.state.opens == 1 && p.state.closes == 1, "Re-enumerate and own exact one session");
    check(p.state.written == byte_vector("ODIN",4), "Exactly four ODIN bytes, no trailing NUL/session command");
    check(p.state.read_capacity == 512, "Read bounded to 512 bytes");
    check(output.vendor_id == 0x04e8 && output.product_id == 0x685d && output.interface_number == 2 &&
        output.bulk_in == 0x83 && output.bulk_out == 4 && output.bulk_in_packet_size == 512 &&
        output.bulk_out_packet_size == 512 && output.handshake_complete == 1 && !output.reserved0 && !output.reserved1,
        "Probe reports only identity, endpoints, packet sizes and completed handshake");
    check(p.state.events == strings("enumerate","open","interfaces","write","read","close"), "No other commands are performed");
    FakePlatform second;
    second.state.reply.push_back('X');
    check(run(second, output) == EJODIN4_OK && second.state.closes == 1 && p.state.closes == 1, "Independent sessions; extended LOKE reply accepted");
    { FakePlatform f; f.state.devices.clear(); failure(f, EJODIN4_NO_DEVICE); check(!f.state.opens, "Missing selection is never opened"); }
    { FakePlatform f; f.state.devices[0] = device("different-interface"); failure(f, EJODIN4_NO_DEVICE); }
    { FakePlatform f; f.state.devices[0].flags = 0; failure(f, EJODIN4_DRIVER_UNSUPPORTED); check(!f.state.opens, "Unsupported driver never opened"); }
    { FakePlatform f; f.state.devices[0].vendor_id = 0x1234; failure(f, EJODIN4_DRIVER_UNSUPPORTED); }
    { FakePlatform f; f.state.devices.push_back(device()); failure(f, EJODIN4_DRIVER_UNSUPPORTED); }
    { FakePlatform f; f.state.enum_status = EJODIN4_USB_IO_ERROR; failure(f, EJODIN4_USB_IO_ERROR); }
    { FakePlatform f; f.state.open_status = EJODIN4_USB_IO_ERROR; failure(f, EJODIN4_USB_IO_ERROR); }
    { FakePlatform f; f.state.interface_status = EJODIN4_USB_IO_ERROR; failure(f, EJODIN4_USB_IO_ERROR); }
    { FakePlatform f; f.state.interfaces.clear(); failure(f, EJODIN4_DRIVER_UNSUPPORTED); check(f.state.written.empty(), "No endpoint means no write"); }
    { FakePlatform f; f.state.interfaces.push_back(bulk(3)); failure(f, EJODIN4_DRIVER_UNSUPPORTED); }
    { FakePlatform f; f.state.interfaces[0].pipes.push_back(extra_in()); failure(f, EJODIN4_DRIVER_UNSUPPORTED); }
    { FakePlatform f; f.state.write_count = 3; failure(f, EJODIN4_USB_IO_ERROR); check(!f.state.read_capacity, "No retry/read after short write"); }
    { FakePlatform f; f.state.write_status = EJODIN4_USB_IO_ERROR; failure(f, EJODIN4_USB_IO_ERROR); }
    { FakePlatform f; f.state.read_status = EJODIN4_USB_IO_ERROR; failure(f, EJODIN4_USB_IO_ERROR); }
    for (unsigned size = 0; size < 4; ++size) { FakePlatform f; f.state.reply.resize(size); failure(f, EJODIN4_BAD_HANDSHAKE); }
    { FakePlatform f; f.state.reply[0] = 'X'; failure(f, EJODIN4_BAD_HANDSHAKE); }
    { FakePlatform f; f.state.reply.resize(513); failure(f, EJODIN4_BAD_HANDSHAKE); }
    { FakePlatform f; failure(f, EJODIN4_CANCELLED, cancel_value(cancel_yes,0)); check(!f.state.enumerations, "Initial cancellation skips enumeration"); }
    { FakePlatform f; f.state.enum_elapsed = 1000; failure(f, EJODIN4_TIMEOUT); check(!f.state.opens, "Deadline during enumeration stops open"); }
    { FakePlatform f; bool flag = false; f.state.cancellation_flag = &flag; f.state.cancel_after_write = true;
      failure(f, EJODIN4_CANCELLED, cancel_value(cancel_flag,&flag)); check(!f.state.read_capacity, "Cancellation after write stops read"); }
    { FakePlatform f; f.state.interfaces.insert(f.state.interfaces.begin(), bulk(1,0xff));
      check(run(f, output) == EJODIN4_OK && output.interface_number == 2, "CDC data pair preferred over vendor interface"); }
    { FakePlatform f; ejodin4::Interface vendor = bulk(1,0xff); vendor.pipes.push_back(extra_in());
      f.state.interfaces.insert(f.state.interfaces.begin(), vendor);
      check(run(f, output) == EJODIN4_OK && output.interface_number == 2, "Unrelated ambiguous vendor endpoints do not valid CDC pair"); }
    { FakePlatform f; f.state.interfaces[0].pipes.push_back(extra_in());
      f.state.interfaces.push_back(bulk(3,0xff)); failure(f, EJODIN4_DRIVER_UNSUPPORTED); }
    { FakePlatform f; f.state.interfaces[0].device_class = 0xff;
      check(run(f, output) == EJODIN4_OK, "Single vendor bulk pair accepted"); }
}
void test_inputs_and_enumeration() {
    check(ejodin4::valid_utf8_path("x", 2), "ASCII bounded path");
    check(ejodin4::valid_utf8_path("\xc3\xa9", 3), "UTF8 nonASCII path");
    const char* invalid[] = {"", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\x80", "\xe2\x82"};
    for (size_t invalidIndex=0;invalidIndex<sizeof(invalid)/sizeof(invalid[0]);++invalidIndex) if(const char* value=invalid[invalidIndex]) check(!ejodin4::valid_utf8_path(value, static_cast<uint32_t>(std::strlen(value) + 1)), "Reject malformed UTF8/empty path");
    check(!ejodin4::valid_utf8_path("a\0b", 4), "Reject interior NUL path");
    check(!ejodin4::valid_utf8_path("x", 1), "Require terminator in capacity");
    check(!ejodin4::valid_utf8_path(0, 0), "Reject null path");
    std::string large(1023, 'x'); check(ejodin4::valid_utf8_path(large.c_str(), 1024), "Maximum path bytes");
    large.push_back('x'); check(!ejodin4::valid_utf8_path(large.c_str(), 1025), "Reject path overflow");
    const uint16_t pids[] = {0x6601,0x685d,0x68c3,0x68ef,0x4eee,0x4eef};
    for (size_t pidIndex=0;pidIndex<sizeof(pids)/sizeof(pids[0]);++pidIndex) if(uint16_t pid=pids[pidIndex]) check(ejodin4::known_samsung(0x04e8,pid), "Known download PID");
    check(!ejodin4::known_samsung(0x04e8,0x6860) && !ejodin4::known_samsung(0x1234,0x685d), "Reject unrelated USB identity");
    FakePlatform p; std::string error; EJODIN4_UsbDevice out[2]; uint32_t count = 99;
    std::memset(out, 0xa5, sizeof(out)); const EJODIN4_UsbDevice untouched = out[0];
    check(ejodin4::enumerate(p,0,0,&count,never,error) == EJODIN4_BUFFER_TOO_SMALL && count == 1, "Enumeration count query");
    check(ejodin4::enumerate(p,out,0,&count,never,error) == EJODIN4_BUFFER_TOO_SMALL && count == 1 &&
        std::memcmp(out,&untouched,sizeof(untouched)) == 0, "Insufficient capacity preserves output");
    error.clear();
    check(ejodin4::enumerate(p,out,2,&count,never,error) == EJODIN4_OK && count == 1 &&
        std::memcmp(&out[0],&p.state.devices[0],sizeof(out[0])) == 0 &&
        std::memcmp(&out[1],&untouched,sizeof(out[1])) == 0, "Enumeration writes only required records");
    p.state.devices[0].flags = 0; p.state.devices[0].device_path[0] = 0;
    const char unsupported_driver[] = "ssudbus";
    std::memcpy(p.state.devices[0].driver, unsupported_driver, sizeof(unsupported_driver));
    check(ejodin4::enumerate(p,out,2,&count,never,error) == EJODIN4_OK && !out[0].flags && !out[0].device_path[0] &&
        std::strcmp(out[0].driver,"ssudbus") == 0, "Unsupported driver record remains truthful");
    count = 99;
    check(ejodin4::enumerate(p,0,1,&count,never,error) == EJODIN4_INVALID_ARGUMENT && !count, "Enumeration invalid output resets count");
    p.state.enum_status = EJODIN4_USB_IO_ERROR; count = 99;
    check(ejodin4::enumerate(p,out,2,&count,never,error) == EJODIN4_USB_IO_ERROR && !count, "Enumeration backend failure resets count");
    p.state.enum_status = EJODIN4_OK; p.state.devices.clear(); count = 99;
    check(ejodin4::enumerate(p,0,0,&count,never,error) == EJODIN4_OK && !count, "No USB devices is successful empty list");
    EJODIN4_UsbProbe probe = {0};
    const uint32_t timeouts[] = {0,30001,UINT32_MAX};
    for (size_t timeoutIndex=0;timeoutIndex<sizeof(timeouts)/sizeof(timeouts[0]);++timeoutIndex) for(uint32_t timeout=timeouts[timeoutIndex],timeoutOnce=1;timeoutOnce;timeoutOnce=0) check(ejodin4::probe(p,"x",2,timeout,&probe,never,error) == EJODIN4_INVALID_ARGUMENT, "Reject invalid timeout");
}
class FakePending : public ejodin4::PendingIo {
public:
    uint64_t clock;
    unsigned waits;
    unsigned ready_after;
    bool completion_ok;
    bool* cancellation_flag;
    std::vector<std::string> events;
    FakePending():clock(0),waits(0),ready_after(1),completion_ok(true),cancellation_flag(0){}
    uint64_t now() const { return clock; }
    bool wait(uint32_t milliseconds) {
        events.push_back("wait"); clock += milliseconds; ++waits;
        if (cancellation_flag) *cancellation_flag = true;
        return waits == ready_after;
    }
    bool completion(uint32_t& actual, std::string& error) {
        events.push_back("complete"); actual = 3; if (!completion_ok) error = "disconnect"; return completion_ok;
    }
    void abort_and_cancel() { events.push_back("abort+cancel"); }
    void drain() { events.push_back("drain"); }
};
void test_serial_deadline_policy(){
    const struct {uint64_t now,until;uint32_t read_ms,write_ms;} cases[]={{100,120100,30000,120000},{100,30100,30000,30000},{100,1100,1000,1000},{100,101,1,1},{0,UINT64_MAX,30000,UINT32_MAX-1u}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){ejodin4::SerialTimeoutPolicy policy;check(ejodin4::serial_timeout_policy(cases[i].now,cases[i].until,policy),"serial timeout policy accepts remaining deadline");check(policy.read_first_byte_ms==cases[i].read_ms && policy.write_total_ms==cases[i].write_ms,"serial first-byte cap and write budget follow independent literal deadline vectors");}
    ejodin4::SerialTimeoutPolicy sentinel;sentinel.read_first_byte_ms=123;sentinel.write_total_ms=456;
    check(!ejodin4::serial_timeout_policy(100,100,sentinel) && sentinel.read_first_byte_ms==123 && sentinel.write_total_ms==456,"expired serial timeout leaves prior policy untouched");
    check(!ejodin4::serial_timeout_policy(101,100,sentinel),"past deadline rejected without unsigned subtraction");
}
void test_pending() {
    uint32_t actual = 99; std::string error;
    { FakePending io; check(ejodin4::finish_pending(io,100,never,actual,error) == EJODIN4_OK && actual == 3,
        "Pending completion retains actual partial byte count"); check(io.events == strings("wait","complete"), "Completed IO needs no abort"); }
    { FakePending io; io.completion_ok = false;
      check(ejodin4::finish_pending(io,100,never,actual,error) == EJODIN4_USB_IO_ERROR, "Pending disconnect status"); }
    { FakePending io; io.ready_after = 999;
      check(ejodin4::finish_pending(io,25,never,actual,error) == EJODIN4_TIMEOUT && actual == 0, "Pending IO obeys deadline");
      check(io.clock == 25 && io.events == strings("wait","wait","abort+cancel","drain"), "Timeout cancels then drains before caller may release buffers"); }
    { FakePending io; bool flag = false; io.cancellation_flag = &flag; io.ready_after = 999;
      check(ejodin4::finish_pending(io,100,cancel_value(cancel_flag,&flag),actual,error) == EJODIN4_CANCELLED && actual == 0, "Pending IO cancellation");
      check(io.events == strings("wait","abort+cancel","drain"), "Cancellation drains exactly once"); }
    { FakePending io; check(ejodin4::finish_pending(io,100,cancel_value(cancel_yes,0),actual,error) == EJODIN4_CANCELLED && !io.waits,
        "Cancellation before wait still drains submitted IO"); }
}
struct Descriptor { uint8_t bAlternateSetting; };
struct AlternateQuery { std::vector<uint8_t>* queried; unsigned* calls; bool endless; AlternateQuery(std::vector<uint8_t>* q,unsigned* c=0,bool e=false):queried(q),calls(c),endless(e){} bool operator()(uint8_t index,Descriptor& result)const {if(calls)++*calls;if(queried)queried->push_back(index);if(endless){result.bAlternateSetting=1;return true;}const uint8_t settings[]={0,3,7};if(index>=3)return false;result.bAlternateSetting=settings[index];return true;} };
void test_alternate_ordinals(){Descriptor d={0};uint8_t ordinal=99;std::vector<uint8_t> queried;AlternateQuery query(&queried);check(ejodin4::find_current_alternate(7,d,ordinal,query)&&ordinal==2&&d.bAlternateSetting==7,"Alternate descriptor ordinal");check(queried.size()==3&&queried[0]==0&&queried[1]==1&&queried[2]==2,"Queries use descriptor ordinal");queried.clear();ordinal=99;check(!ejodin4::find_current_alternate(6,d,ordinal,query)&&ordinal==99,"Missing alternate does not invent selection");check(queried.size()==4&&queried[3]==3,"Enumeration stops at native end");unsigned calls=0;check(!ejodin4::find_current_alternate(2,d,ordinal,AlternateQuery(0,&calls,true))&&calls==256,"Alternate query is bounded");}

}
int main() {
    try { test_probe(); test_inputs_and_enumeration(); test_pending(); test_alternate_ordinals(); test_serial_deadline_policy();
        std::cout << "Native BCC fake USB transport tests passed: " << checks << " checks\n"; return 0;
    } catch (const std::exception& e) { std::cerr << "Odin4 fake USB failure after " << checks << ": " << e.what() << '\n'; return 1; }
}
