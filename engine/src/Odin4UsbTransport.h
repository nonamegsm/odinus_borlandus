#ifndef EASYJTAG_ODIN4_USB_TRANSPORT_H
#define EASYJTAG_ODIN4_USB_TRANSPORT_H

#include "EasyJtagOdin4Core.h"
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

// Private injection boundary. Not exported from the DLL and never selected by
// production settings: tests construct an independent fake Platform directly.
namespace ejodin4 {
struct Cancellation {
    EJODIN4_CancelCallback callback;
    void* context;
    bool requested() const { return callback && callback(context) != 0; }
};
struct Pipe {
    uint8_t address;
    uint16_t packet_size;
    bool bulk;
};
struct Interface {
    uint8_t number;
    uint8_t device_class;
    std::vector<Pipe> pipes;
};
struct Selection {
    size_t interface_index;
    uint8_t in;
    uint8_t out;
    uint16_t in_packet;
    uint16_t out_packet;
};
// WinUSB descriptors are indexed by ordinal; active bAlternateSetting values
// need not be contiguous. Query returns false at the native enumeration end or
// on error; the native caller retains its exact error/cancellation status.
template<class Descriptor, class Query>
bool find_current_alternate(uint8_t current, Descriptor& descriptor,
    uint8_t& selected_ordinal, Query query) {
    for (unsigned index = 0; index < 256; ++index) {
        if (!query(static_cast<uint8_t>(index), descriptor)) return false;
        if (descriptor.bAlternateSetting == current) {
            selected_ordinal = static_cast<uint8_t>(index);
            return true;
        }
    }
    return false;
}
class Connection {
public:
    virtual ~Connection() {}
    virtual EJODIN4_Status interfaces(std::vector<Interface>& result, std::string& error) = 0;
    virtual EJODIN4_Status transfer(size_t interface_index, uint8_t pipe,
        uint8_t* data, uint32_t capacity, uint32_t& actual, uint64_t deadline,
        const Cancellation& cancel, std::string& error) = 0;
};
class Platform {
public:
    virtual ~Platform() {}
    virtual uint64_t now() const = 0;
    virtual EJODIN4_Status enumerate(std::vector<EJODIN4_UsbDevice>& devices,
        const Cancellation& cancel, std::string& error) = 0;
    virtual EJODIN4_Status open(const EJODIN4_UsbDevice& device,
        std::unique_ptr<Connection>& connection, uint64_t deadline,
        const Cancellation& cancel, std::string& error) = 0;
    virtual EJODIN4_Status open_serial(const EJODIN4_UsbDevice&,
        std::unique_ptr<class SerialConnection>&, uint64_t,
        const Cancellation&, std::string& error) {
        error = "Samsung serial transport is unavailable";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
};

// A pending operation owns no caller buffer. The concrete caller keeps the
// OVERLAPPED, event, connection and buffer alive through drain(). drain() must
// wait for kernel completion, even if an unhealthy driver ignores cancellation.
class PendingIo {
public:
    virtual ~PendingIo() {}
    virtual uint64_t now() const = 0;
    virtual bool wait(uint32_t milliseconds) = 0;
    virtual bool completion(uint32_t& actual, std::string& error) = 0;
    virtual void abort_and_cancel() = 0;
    virtual void drain() = 0;
};
EJODIN4_Status finish_pending(PendingIo& io, uint64_t deadline,
    const Cancellation& cancel, uint32_t& actual, std::string& error);
bool known_samsung(uint16_t vendor, uint16_t product);
bool valid_utf8_path(const char* path, uint32_t bytes);
EJODIN4_Status select_bulk(const std::vector<Interface>& interfaces,
    Selection& selection, std::string& error);
EJODIN4_Status enumerate(Platform& platform, EJODIN4_UsbDevice* devices,
    uint32_t capacity, uint32_t* count, const Cancellation& cancel, std::string& error);
EJODIN4_Status probe(Platform& platform, const char* path, uint32_t bytes,
    uint32_t timeout, EJODIN4_UsbProbe* output, const Cancellation& cancel,
    std::string& error);
std::unique_ptr<Platform> make_native_platform(bool include_serial = false);
}
#endif
