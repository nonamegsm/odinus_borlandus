#include "Odin4DownloadChannel.h"
#include <algorithm>
#include <cstring>
#include <limits>
namespace ejodin4 {
namespace {
uint64_t deadline(Platform& p, uint32_t timeout) {
    const uint64_t now = p.now();
    return now > UINT64_MAX - timeout ? UINT64_MAX : now + timeout;
}
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
}
DownloadChannel::DownloadChannel(Platform& p, Cancellation c) : platform_(p), cancel_(c), timeout_(30000), opened_(false) { std::memset(&identity_,0,sizeof(identity_)); std::memset(&pipes_,0,sizeof(pipes_)); }
DownloadChannel::~DownloadChannel() { std::string ignored; finish(ignored); }
EJODIN4_Status DownloadChannel::interrupted(std::string& error) const {
    if (cancel_.requested()) { error = "Download session cancelled"; return EJODIN4_CANCELLED; }
    return EJODIN4_OK;
}
EJODIN4_Status DownloadChannel::open(const char* path, uint32_t bytes, uint32_t timeout, std::string& error) {
    if (opened_ || !valid_utf8_path(path, bytes) || !timeout || timeout > 120000) {
        error = "Download channel needs one exact interface and timeout 1..120000 ms";
        return EJODIN4_INVALID_ARGUMENT;
    }
    EJODIN4_Status status = interrupted(error);
    if (status != EJODIN4_OK) return status;
    const uint64_t until = deadline(platform_, timeout);
    std::vector<EJODIN4_UsbDevice> devices;
    status = platform_.enumerate(devices, cancel_, error);
    if (status != EJODIN4_OK) return status;
    const EJODIN4_UsbDevice* selected = 0;
    for (size_t device_index = 0; device_index < devices.size(); ++device_index) {
        const EJODIN4_UsbDevice& d = devices[device_index];
        if (!std::memchr(d.device_path, 0, sizeof(d.device_path))) {
            error = "Download enumeration returned an unterminated identity"; return EJODIN4_INTERNAL_ERROR;
        }
        if (std::strcmp(d.device_path, path)) continue;
        if (selected) { error = "Selected Download interface is ambiguous"; return EJODIN4_DRIVER_UNSUPPORTED; }
        selected = &d;
    }
    if (!selected) { error = "Selected Download interface is no longer present"; return EJODIN4_NO_DEVICE; }
    if (!known_samsung(selected->vendor_id, selected->product_id) ||
        (selected->flags != EJODIN4_DOWNLOAD_WINUSB && selected->flags != EJODIN4_DOWNLOAD_SAMSUNG_SERIAL)) {
        error = "Selected interface has no supported Samsung Download transport"; return EJODIN4_DRIVER_UNSUPPORTED;
    }
    if ((status = interrupted(error)) != EJODIN4_OK) return status;
    if (platform_.now() >= until) { error = "Download interface discovery timed out"; return EJODIN4_TIMEOUT; }
    identity_ = *selected;
    timeout_ = timeout;
    if (identity_.flags == EJODIN4_DOWNLOAD_SAMSUNG_SERIAL) {
        status = platform_.open_serial(identity_, serial_, until, cancel_, error);
        if (status == EJODIN4_OK && !serial_.get()) { error = "Missing serial connection"; status = EJODIN4_INTERNAL_ERROR; }
    } else {
        status = platform_.open(identity_, usb_, until, cancel_, error);
        if (status == EJODIN4_OK && !usb_.get()) { error = "Missing WinUSB connection"; status = EJODIN4_INTERNAL_ERROR; }
        if (status == EJODIN4_OK) {
            std::vector<Interface> interfaces;
            status = usb_->interfaces(interfaces, error);
            if (status == EJODIN4_OK) status = select_bulk(interfaces, pipes_, error);
        }
    }
    if (status != EJODIN4_OK) { std::string ignored; finish(ignored); return status; }
    opened_ = true;
    return EJODIN4_OK;
}
EJODIN4_Status DownloadChannel::exact(bool read, uint8_t* data, uint32_t bytes, uint32_t timeout, std::string& error) {
    if (!opened_ || !data || !bytes || !timeout) { error = "Invalid Download transfer"; return EJODIN4_INVALID_ARGUMENT; }
    const uint64_t until = deadline(platform_, timeout);
    uint32_t offset = 0;
    while (offset < bytes) {
        EJODIN4_Status status = interrupted(error);
        if (status != EJODIN4_OK) return status;
        if (platform_.now() >= until) { error = "Download transfer timed out"; return EJODIN4_TIMEOUT; }
        uint32_t actual = 0;
        const uint64_t before = platform_.now();
        status = serial_.get() ? serial_->transfer(read, data + offset, bytes - offset, actual, until, cancel_, error) :
            usb_->transfer(pipes_.interface_index, read ? pipes_.in : pipes_.out,
                data + offset, bytes - offset, actual, until, cancel_, error);
        if (status != EJODIN4_OK) return status;
        if (cancel_.requested()) { error = "Download session cancelled"; return EJODIN4_CANCELLED; }
        if (!actual && read && serial_.get() && platform_.now() > before) continue;
        if (!actual || actual > bytes - offset) { error = "Invalid or empty Download transfer"; return EJODIN4_USB_IO_ERROR; }
        offset += actual;
    }
    // USB packet boundaries are not bytes and must never be emulated on serial.
    if (!read && usb_.get() && bytes % pipes_.out_packet == 0) return usb_boundary(false, error);
    return EJODIN4_OK;
}
EJODIN4_Status DownloadChannel::read_exact(uint8_t* p,uint32_t n,uint32_t t,std::string& e) { return exact(true,p,n,t,e); }
EJODIN4_Status DownloadChannel::write_exact(const uint8_t* p,uint32_t n,uint32_t t,std::string& e) {
    return exact(false,const_cast<uint8_t*>(p),n,t,e);
}
EJODIN4_Status DownloadChannel::handshake(std::string& error) {
    static const uint8_t hello[] = {'O','D','I','N'};
    EJODIN4_Status s = write_exact(hello,4,timeout_,error);
    if (s != EJODIN4_OK) return s;
    uint8_t reply[4] = {0};
    s = read_exact(reply,4,timeout_,error);
    if (s == EJODIN4_OK && std::memcmp(reply,"LOKE",4)) { error = "Download handshake did not return LOKE"; return EJODIN4_BAD_HANDSHAKE; }
    return s;
}
EJODIN4_Status DownloadChannel::optional_pit_ack(uint32_t& id,uint32_t& ack,bool& present,std::string& error) {
    present = false;
    if (!serial_.get()) return EJODIN4_OK;
    uint8_t bytes[8] = {0};
    const uint64_t until = deadline(platform_,200);
    EJODIN4_Status s = serial_->read_optional_frame(bytes,8,present,until,cancel_,error);
    if (s != EJODIN4_OK) return s;
    if (!present) return EJODIN4_OK;
    id = le32(bytes); ack = le32(bytes+4);
    return EJODIN4_OK;
}
EJODIN4_Status DownloadChannel::read_metadata_ack(uint8_t* ack,uint32_t timeout,
    bool& tail_present,uint32_t& tail_word,std::string& error) {
    tail_present=false;tail_word=0;
    if(serial_.get()) {
        EJODIN4_Status s=read_exact(ack,8,timeout,error);
        if(s!=EJODIN4_OK)return s;
        uint8_t tail[4] = {0};
        s=serial_->read_optional_frame(tail,4,tail_present,deadline(platform_,200),cancel_,error);
        if(s!=EJODIN4_OK)return s;
        if(tail_present)tail_word=le32(tail);
        return EJODIN4_OK;
    }
    if(!opened_ || !usb_.get()) { error="Download metadata channel is not open";return EJODIN4_INVALID_ARGUMENT; }
    uint8_t response[512] = {0};uint32_t actual=0;
    const EJODIN4_Status s=usb_->transfer(pipes_.interface_index,pipes_.in,response,sizeof(response),
        actual,deadline(platform_,timeout),cancel_,error);
    if(s!=EJODIN4_OK)return s;
    if(actual!=8 && actual!=12) {error="Unexpected Samsung metadata response size";return EJODIN4_INVALID_RESPONSE;}
    std::memcpy(ack,response,8);
    tail_present=actual==12;if(tail_present)tail_word=le32(response+8);
    return EJODIN4_OK;
}
EJODIN4_Status DownloadChannel::usb_boundary(bool read,std::string& error) {
    if (!usb_.get()) return EJODIN4_OK;
    uint8_t dummy = 0;
    uint32_t actual = 0;
    // Empty IN is an optional one-byte receive, as in pinned odin4. A zero
    // capacity WinUSB read need not consume a pending endpoint boundary.
    EJODIN4_Status s = usb_->transfer(pipes_.interface_index,read?pipes_.in:pipes_.out,
        &dummy,read?1u:0u,actual,deadline(platform_,read?100u:timeout_),cancel_,error);
    if (read && s == EJODIN4_TIMEOUT && !actual && !cancel_.requested()) { error.clear(); return EJODIN4_OK; }
    if (s == EJODIN4_OK && actual) { error = "Unexpected USB boundary payload"; return EJODIN4_USB_IO_ERROR; }
    return s;
}
EJODIN4_Status DownloadChannel::finish(std::string& error) {
    EJODIN4_Status s = EJODIN4_OK;
    if(serial_.get()) s = serial_->finish(error);
    serial_.reset(); usb_.reset(); opened_ = false;
    return s;
}
}
