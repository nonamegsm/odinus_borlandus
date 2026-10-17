#include "Odin4UsbTransport.h"
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

static_assert(sizeof(EJODIN4_UsbDevice) == 1608, "USB device ABI");
static_assert(sizeof(EJODIN4_UsbProbe) == 14, "USB probe ABI");

namespace ejodin4 {
namespace {
struct ProbeInterruption {
    Platform& platform; const Cancellation& cancel; uint64_t deadline; std::string& error;
    ProbeInterruption(Platform& p,const Cancellation& c,uint64_t d,std::string& e):platform(p),cancel(c),deadline(d),error(e) {}
    EJODIN4_Status operator()() const {
        if(cancel.requested()){error="USB probe cancelled";return EJODIN4_CANCELLED;}
        if(platform.now()>=deadline){error="USB probe timed out";return EJODIN4_TIMEOUT;}
        return EJODIN4_OK;
    }
};
}

bool known_samsung(uint16_t vendor, uint16_t product) {
    if (vendor != 0x04e8) return false;
    switch (product) {
    case 0x6601: case 0x685d: case 0x68c3: case 0x68ef: case 0x4eee: case 0x4eef: return true;
    default: return false;
    }
}

bool valid_utf8_path(const char* path, uint32_t bytes) {
    if (!path || bytes < 2 || bytes > 1024 || path[bytes - 1] != 0) return false;
    uint32_t i = 0;
    while (i < bytes - 1) {
        const uint8_t first = static_cast<uint8_t>(path[i++]);
        if (!first) return false;
        if (first < 0x80) continue;
        unsigned continuation;
        uint32_t code;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) { continuation = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { continuation = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { continuation = 3; code = first & 7; minimum = 0x10000; }
        else return false;
        if (continuation > bytes - 1 - i) return false;
        while (continuation--) {
            const uint8_t next = static_cast<uint8_t>(path[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

EJODIN4_Status finish_pending(PendingIo& io, uint64_t deadline,
    const Cancellation& cancel, uint32_t& actual, std::string& error) {
    actual = 0;
    for (;;) {
        EJODIN4_Status reason = EJODIN4_OK;
        if (cancel.requested()) reason = EJODIN4_CANCELLED;
        const uint64_t now = io.now();
        if (reason == EJODIN4_OK && now >= deadline) reason = EJODIN4_TIMEOUT;
        if (reason != EJODIN4_OK) {
            io.abort_and_cancel();
            io.drain();
            error = reason == EJODIN4_CANCELLED ? "USB transfer cancelled; pending I/O drained" : "USB transfer timed out; pending I/O drained";
            return reason;
        }
        const uint64_t remaining = deadline - now;
        const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(remaining, 20));
        if (io.wait(slice)) return io.completion(actual, error) ? EJODIN4_OK : EJODIN4_USB_IO_ERROR;
    }
}

EJODIN4_Status select_bulk(const std::vector<Interface>& interfaces,
    Selection& selection, std::string& error) {
    std::vector<Selection> preferred, fallback;
    bool preferred_ambiguous = false, fallback_ambiguous = false;
    for (size_t index = 0; index < interfaces.size(); ++index) {
        const Interface& item = interfaces[index];
        Selection candidate = {index, 0, 0, 0, 0};
        unsigned ins = 0, outs = 0;
        for (size_t pipe_index = 0; pipe_index < item.pipes.size(); ++pipe_index) {
            const Pipe& pipe = item.pipes[pipe_index];
            if (!pipe.bulk || !pipe.packet_size) continue;
            if (pipe.address & 0x80) { ++ins; candidate.in = pipe.address; candidate.in_packet = pipe.packet_size; }
            else if (pipe.address) { ++outs; candidate.out = pipe.address; candidate.out_packet = pipe.packet_size; }
        }
        if (!ins || !outs) continue;
        if (ins != 1 || outs != 1) {
            (item.device_class == 0x0a ? preferred_ambiguous : fallback_ambiguous) = true;
            continue;
        }
        (item.device_class == 0x0a ? preferred : fallback).push_back(candidate);
    }
    const bool use_preferred = preferred_ambiguous || !preferred.empty();
    const std::vector<Selection>& choices = use_preferred ? preferred : fallback;
    if (use_preferred ? preferred_ambiguous : fallback_ambiguous) {
        error = "USB interface contains ambiguous bulk endpoints";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
    if (choices.size() != 1) {
        error = choices.empty() ? "No USB interface has a bulk IN/OUT pair" : "Multiple USB interfaces have candidate bulk pairs";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
    selection = choices.front();
    return EJODIN4_OK;
}

EJODIN4_Status enumerate(Platform& platform, EJODIN4_UsbDevice* devices,
    uint32_t capacity, uint32_t* count, const Cancellation& cancel, std::string& error) {
    if (count) *count = 0;
    if (!count || (!devices && capacity)) { error = "Invalid USB enumeration output"; return EJODIN4_INVALID_ARGUMENT; }
    if (cancel.requested()) { error = "USB enumeration cancelled"; return EJODIN4_CANCELLED; }
    std::vector<EJODIN4_UsbDevice> found;
    const EJODIN4_Status status = platform.enumerate(found, cancel, error);
    if (status != EJODIN4_OK) return status;
    if (cancel.requested()) { error = "USB enumeration cancelled"; return EJODIN4_CANCELLED; }
    if (found.size() > std::numeric_limits<uint32_t>::max()) { error = "Too many USB records"; return EJODIN4_INTERNAL_ERROR; }
    const uint32_t required = static_cast<uint32_t>(found.size());
    if (capacity < required) { *count = required; error = "USB device capacity is insufficient"; return EJODIN4_BUFFER_TOO_SMALL; }
    if (required) std::memcpy(devices, &found[0], required * sizeof(*devices));
    *count = required;
    return EJODIN4_OK;
}

EJODIN4_Status probe(Platform& platform, const char* path, uint32_t bytes,
    uint32_t timeout, EJODIN4_UsbProbe* output, const Cancellation& cancel,
    std::string& error) {
    if (!output || !valid_utf8_path(path, bytes) || !timeout || timeout > 30000) {
        error = "USB probe needs a bounded UTF-8 interface path and timeout 1..30000 ms";
        return EJODIN4_INVALID_ARGUMENT;
    }
    const uint64_t start = platform.now();
    const uint64_t deadline = start > UINT64_MAX - timeout ? UINT64_MAX : start + timeout;
    ProbeInterruption interrupted(platform,cancel,deadline,error);
    EJODIN4_Status status = interrupted();
    if (status != EJODIN4_OK) return status;
    std::vector<EJODIN4_UsbDevice> devices;
    status = platform.enumerate(devices, cancel, error);
    if (status != EJODIN4_OK) return status;
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    const EJODIN4_UsbDevice* selected = 0;
    for (size_t device_index = 0; device_index < devices.size(); ++device_index) {
        const EJODIN4_UsbDevice& candidate = devices[device_index];
        if (std::strcmp(candidate.device_path, path) != 0) continue;
        if (selected) { error = "Selected USB identity is ambiguous"; return EJODIN4_DRIVER_UNSUPPORTED; }
        selected = &candidate;
    }
    if (!selected) { error = "Selected USB interface is no longer present"; return EJODIN4_NO_DEVICE; }
    if (!known_samsung(selected->vendor_id, selected->product_id) || !(selected->flags & EJODIN4_USB_WINUSB_COMPATIBLE)) {
        error = "Selected Samsung interface is not bound to WinUSB";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
    std::unique_ptr<Connection> connection;
    status = platform.open(*selected, connection, deadline, cancel, error);
    if (status != EJODIN4_OK) return status;
    if (!connection) { error = "USB platform returned no connection"; return EJODIN4_INTERNAL_ERROR; }
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    std::vector<Interface> interfaces;
    status = connection->interfaces(interfaces, error);
    if (status != EJODIN4_OK) return status;
    Selection pipes;
    status = select_bulk(interfaces, pipes, error);
    if (status != EJODIN4_OK) return status;
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    uint8_t hello[] = {'O', 'D', 'I', 'N'};
    uint32_t actual = 0;
    status = connection->transfer(pipes.interface_index, pipes.out, hello, 4, actual, deadline, cancel, error);
    if (status != EJODIN4_OK) return status;
    if (actual != 4) { error = "Short ODIN handshake write"; return EJODIN4_USB_IO_ERROR; }
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    uint8_t reply[512] = {0};
    status = connection->transfer(pipes.interface_index, pipes.in, reply, sizeof(reply), actual, deadline, cancel, error);
    if (status != EJODIN4_OK) return status;
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    if (actual < 4 || actual > sizeof(reply) || std::memcmp(reply, "LOKE", 4) != 0) {
        error = "USB handshake did not return a LOKE prefix";
        return EJODIN4_BAD_HANDSHAKE;
    }
    const EJODIN4_UsbProbe success = {selected->vendor_id, selected->product_id,
        interfaces[pipes.interface_index].number, pipes.in, pipes.out, 0,
        pipes.in_packet, pipes.out_packet, 1, 0};
    std::memcpy(output, &success, sizeof(success));
    return EJODIN4_OK;
}
}

namespace {
EJODIN4_Status usb_result(EJODIN4_Status status, const char* message,
    char* diagnostic, uint32_t capacity) {
    if (diagnostic && capacity) {
        const size_t copied = std::min<size_t>(std::strlen(message), capacity - 1);
        std::memcpy(diagnostic, message, copied);
        diagnostic[copied] = 0;
    }
    return status;
}
}

EJODIN4_Status EJODIN4_CALL EJODIN4_EnumerateUsbDevices(
    EJODIN4_UsbDevice* devices, uint32_t capacity, uint32_t* count,
    EJODIN4_CancelCallback cancel, void* context, char* diagnostic, uint32_t diagnostic_capacity) {
    if (count) *count = 0;
    try {
        if ((!diagnostic && diagnostic_capacity) || !count || (!devices && capacity))
            return usb_result(EJODIN4_INVALID_ARGUMENT, "Invalid USB enumeration arguments", diagnostic, diagnostic_capacity);
        const ejodin4::Cancellation cancellation = {cancel, context};
        if (cancellation.requested()) return usb_result(EJODIN4_CANCELLED, "USB enumeration cancelled", diagnostic, diagnostic_capacity);
        std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform();
        std::string error;
        const EJODIN4_Status status = ejodin4::enumerate(*platform, devices, capacity, count, cancellation, error);
        return usb_result(status, error.c_str(), diagnostic, diagnostic_capacity);
    } catch (const std::exception& error) {
        if (count) *count = 0;
        return usb_result(EJODIN4_INTERNAL_ERROR, error.what(), diagnostic, diagnostic_capacity);
    } catch (...) {
        if (count) *count = 0;
        return usb_result(EJODIN4_INTERNAL_ERROR, "USB enumeration raised an exception", diagnostic, diagnostic_capacity);
    }
}

EJODIN4_Status EJODIN4_CALL EJODIN4_ProbeUsbDevice(
    const char* path, uint32_t bytes, uint32_t timeout, EJODIN4_UsbProbe* output,
    EJODIN4_CancelCallback cancel, void* context, char* diagnostic, uint32_t diagnostic_capacity) {
    try {
        if ((!diagnostic && diagnostic_capacity) || !output || !ejodin4::valid_utf8_path(path, bytes) || !timeout || timeout > 30000)
            return usb_result(EJODIN4_INVALID_ARGUMENT, "Invalid USB probe arguments", diagnostic, diagnostic_capacity);
        const ejodin4::Cancellation cancellation = {cancel, context};
        if (cancellation.requested()) return usb_result(EJODIN4_CANCELLED, "USB probe cancelled", diagnostic, diagnostic_capacity);
        std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform();
        std::string error;
        const EJODIN4_Status status = ejodin4::probe(*platform, path, bytes, timeout, output, cancellation, error);
        return usb_result(status, error.c_str(), diagnostic, diagnostic_capacity);
    } catch (const std::exception& error) {
        return usb_result(EJODIN4_INTERNAL_ERROR, error.what(), diagnostic, diagnostic_capacity);
    } catch (...) {
        return usb_result(EJODIN4_INTERNAL_ERROR, "USB probe raised an exception", diagnostic, diagnostic_capacity);
    }
}
