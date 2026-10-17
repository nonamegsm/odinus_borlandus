#include "Odin4SamsungSerial.h"
// Reuse the supported Windows API declarations missing from TDM's headers.
#include "Odin4WinUsbApi.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <exception>
#include <stdexcept>

static_assert(sizeof(EJODIN4_DownloadProbe) == 12, "Download probe ABI");
namespace ejodin4 {
namespace {
struct DownloadInterruption {
    Platform& platform;const Cancellation& cancel;uint64_t deadline;std::string& error;
    DownloadInterruption(Platform& p,const Cancellation& c,uint64_t d,std::string& e):platform(p),cancel(c),deadline(d),error(e) {}
    EJODIN4_Status operator()() const {
        if(cancel.requested()){error="Download handshake cancelled";return EJODIN4_CANCELLED;}
        if(platform.now()>=deadline){error="Download handshake timed out";return EJODIN4_TIMEOUT;}
        return EJODIN4_OK;
    }
};
}

bool valid_com_port(const std::wstring& port) {
    if (port.size() < 4 || port.size() > 8 || port.compare(0, 3, L"COM") || port[3] == L'0') return false;
    uint32_t number = 0;
    for (size_t i = 3; i < port.size(); ++i) {
        if (port[i] < L'0' || port[i] > L'9') return false;
        number = number * 10 + static_cast<uint32_t>(port[i] - L'0');
    }
    return number > 0 && number <= 65535;
}

EJODIN4_Status probe_download(Platform& platform, const char* path, uint32_t bytes,
    uint32_t timeout, EJODIN4_DownloadProbe* output, const Cancellation& cancel,
    std::string& error) {
    if (!output || !valid_utf8_path(path, bytes) || !timeout || timeout > 30000) {
        error = "Download probe needs a bounded UTF-8 interface path and timeout 1..30000 ms";
        return EJODIN4_INVALID_ARGUMENT;
    }
    const uint64_t start = platform.now();
    const uint64_t deadline = start > UINT64_MAX - timeout ? UINT64_MAX : start + timeout;
    DownloadInterruption interrupted(platform,cancel,deadline,error);
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
        if (std::strcmp(candidate.device_path, path)) continue;
        if (selected) { error = "Selected Download interface identity is ambiguous"; return EJODIN4_DRIVER_UNSUPPORTED; }
        selected = &candidate;
    }
    if (!selected) { error = "Selected Download interface is no longer present"; return EJODIN4_NO_DEVICE; }
    if (!known_samsung(selected->vendor_id, selected->product_id) ||
        (selected->flags != EJODIN4_DOWNLOAD_WINUSB && selected->flags != EJODIN4_DOWNLOAD_SAMSUNG_SERIAL)) {
        error = "Selected Samsung interface has no supported Download transport";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
    EJODIN4_DownloadProbe success = {selected->vendor_id, selected->product_id, selected->flags, 1, {0,0,0}};
    if (selected->flags == EJODIN4_DOWNLOAD_WINUSB) {
        status = interrupted();
        if (status != EJODIN4_OK) return status;
        const uint64_t remaining = deadline - platform.now();
        if (!remaining || remaining > timeout) { error = "Download handshake timed out"; return EJODIN4_TIMEOUT; }
        EJODIN4_UsbProbe usb = {0};
        status = probe(platform, path, bytes, static_cast<uint32_t>(remaining), &usb, cancel, error);
        if (status != EJODIN4_OK) return status;
        if (usb.vendor_id != selected->vendor_id || usb.product_id != selected->product_id || !usb.handshake_complete) {
            error = "Download interface changed during WinUSB verification"; return EJODIN4_NO_DEVICE;
        }
    } else {
        std::unique_ptr<SerialConnection> connection;
        status = platform.open_serial(*selected, connection, deadline, cancel, error);
        if (status != EJODIN4_OK) return status;
        if (!connection) { error = "Samsung serial platform returned no connection"; return EJODIN4_INTERNAL_ERROR; }
        uint8_t hello[4] = {'O','D','I','N'}, reply[4] = {0};
        for (unsigned phase = 0; phase != 2; ++phase) {
            uint32_t offset = 0;
            while (offset != 4) {
                status = interrupted();
                if (status != EJODIN4_OK) return status;
                uint32_t actual = 0;
                status = connection->transfer(phase != 0, (phase ? reply : hello) + offset,
                    4 - offset, actual, deadline, cancel, error);
                if (status != EJODIN4_OK) return status;
                if (actual > 4 - offset) { error = "Invalid Samsung serial transfer length"; return EJODIN4_USB_IO_ERROR; }
                if (!actual) { error = "Samsung serial transfer made no progress"; return EJODIN4_USB_IO_ERROR; }
                offset += actual;
            }
        }
        if (std::memcmp(reply, "LOKE", 4)) { error = "Samsung serial handshake did not return LOKE"; return EJODIN4_BAD_HANDSHAKE; }
        status = connection->finish(error);
        if (status != EJODIN4_OK) return status;
    }
    status = interrupted();
    if (status != EJODIN4_OK) return status;
    std::memcpy(output, &success, sizeof(success));
    return EJODIN4_OK;
}

namespace {
std::string serial_error(const char* action, DWORD code = GetLastError()) {
    char number[32]; std::sprintf(number,"%lu",(unsigned long)code);
    return std::string(action) + " (Windows error " + number + ")";
}
class SerialPending : public PendingIo {
    HANDLE file_;
    bool active_;
    DWORD wait_error_;
public:
    OVERLAPPED overlapped;
    explicit SerialPending(HANDLE file) : file_(file), active_(false), wait_error_(0) {
        ZeroMemory(&overlapped,sizeof(overlapped));
        overlapped.hEvent = CreateEventW(0, TRUE, FALSE, 0);
        if (!overlapped.hEvent) throw std::runtime_error(serial_error("Create Samsung serial completion event"));
    }
    ~SerialPending() {
        if (active_) { abort_and_cancel(); drain(); }
        CloseHandle(overlapped.hEvent);
    }
    void submitted() { active_ = true; }
    uint64_t now() const { return GetTickCount64(); }
    bool wait(uint32_t milliseconds) {
        const DWORD outcome = WaitForSingleObject(overlapped.hEvent, milliseconds);
        if (outcome == WAIT_FAILED) { wait_error_ = GetLastError(); return true; }
        return outcome == WAIT_OBJECT_0;
    }
    bool completion(uint32_t& actual, std::string& error) {
        if (wait_error_) {
            abort_and_cancel(); drain(); error = serial_error("Wait for Samsung serial transfer", wait_error_); return false;
        }
        DWORD transferred = 0;
        const BOOL ok = GetOverlappedResult(file_, &overlapped, &transferred, FALSE);
        const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (!HasOverlappedIoCompleted(&overlapped)) { abort_and_cancel(); drain(); }
        else active_ = false;
        actual = static_cast<uint32_t>(transferred);
        if (!ok) error = serial_error("Complete Samsung serial transfer", code);
        return ok != FALSE;
    }
    void abort_and_cancel() { CancelIoEx(file_, &overlapped); }
    void drain() {
        if (!active_) return;
        DWORD transferred = 0;
        do { GetOverlappedResult(file_, &overlapped, &transferred, TRUE); }
        while (!HasOverlappedIoCompleted(&overlapped));
        active_ = false;
    }
};
class NativeSerial : public SerialConnection {
    HANDLE file_;
    COMMTIMEOUTS original_;
    bool restore_;
public:
    NativeSerial() : file_(INVALID_HANDLE_VALUE), restore_(false) { ZeroMemory(&original_,sizeof(original_)); }
    ~NativeSerial() {
        if (file_ != INVALID_HANDLE_VALUE) {
            if (restore_) SetCommTimeouts(file_, &original_);
            CloseHandle(file_);
        }
    }
    EJODIN4_Status open(const EJODIN4_UsbDevice& device, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        if (cancel.requested()) { error = "Samsung serial open cancelled"; return EJODIN4_CANCELLED; }
        if (GetTickCount64() >= deadline) { error = "Samsung serial open timed out"; return EJODIN4_TIMEOUT; }
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.device_path, -1, 0, 0);
        if (!count) { error = "Invalid Samsung serial interface encoding"; return EJODIN4_INTERNAL_ERROR; }
        std::vector<wchar_t> path(static_cast<size_t>(count));
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, device.device_path, -1, (&path[0]), count)) {
            error = serial_error("Convert Samsung serial interface"); return EJODIN4_INTERNAL_ERROR;
        }
        file_ = CreateFileW((&path[0]), GENERIC_READ | GENERIC_WRITE, 0, 0,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, 0);
        if (file_ == INVALID_HANDLE_VALUE) { error = serial_error("Open selected Samsung modem interface (device may be busy)"); return EJODIN4_USB_IO_ERROR; }
        if (!GetCommTimeouts(file_, &original_)) { error = serial_error("Read Samsung communication timeouts"); return EJODIN4_USB_IO_ERROR; }
        COMMTIMEOUTS bounded = {0};
        // Return available bytes or wait at most the total caller timeout for
        // the first byte; finish_pending enforces its finer deadline/cancel.
        bounded.ReadIntervalTimeout = MAXDWORD;
        bounded.ReadTotalTimeoutMultiplier = MAXDWORD;
        bounded.ReadTotalTimeoutConstant = 30000;
        bounded.WriteTotalTimeoutConstant = 30000;
        restore_ = true;
        if (!SetCommTimeouts(file_, &bounded)) { error = serial_error("Set bounded Samsung communication timeouts"); return EJODIN4_USB_IO_ERROR; }
        return EJODIN4_OK;
    }
    EJODIN4_Status transfer(bool read, uint8_t* data, uint32_t capacity,
        uint32_t& actual, uint64_t deadline, const Cancellation& cancel, std::string& error) {
        actual = 0;
        if (cancel.requested()) { error = "Samsung serial transfer cancelled"; return EJODIN4_CANCELLED; }
        if (GetTickCount64() >= deadline) { error = "Samsung serial transfer timed out"; return EJODIN4_TIMEOUT; }
        SerialPending pending(file_);
        DWORD transferred = 0;
        const BOOL ok = read ? ReadFile(file_, data, capacity, &transferred, &pending.overlapped)
            : WriteFile(file_, data, capacity, &transferred, &pending.overlapped);
        if (ok) { actual = static_cast<uint32_t>(transferred); return EJODIN4_OK; }
        const DWORD code = GetLastError();
        if (code != ERROR_IO_PENDING) { error = serial_error("Submit Samsung serial transfer", code); return EJODIN4_USB_IO_ERROR; }
        pending.submitted();
        return finish_pending(pending, deadline, cancel, actual, error);
    }
    EJODIN4_Status read_optional_frame(uint8_t* bytes, uint32_t size, bool& present, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        present = false;
        if (!bytes || (size != 4 && size != 8)) { error = "Invalid optional Samsung frame size"; return EJODIN4_INVALID_ARGUMENT; }
        for (;;) {
            if (cancel.requested()) { error = "Optional Samsung frame cancelled"; return EJODIN4_CANCELLED; }
            DWORD flags = 0;
            COMSTAT state = {0};
            if (!ClearCommError(file_, &flags, &state)) {
                error = serial_error("Inspect queued Samsung frame"); return EJODIN4_USB_IO_ERROR;
            }
            if (flags) { error = "Samsung serial communication errors while inspecting optional frame"; return EJODIN4_USB_IO_ERROR; }
            if (state.cbInQue > size) { error = "Unexpected queued bytes after Samsung control response"; return EJODIN4_USB_IO_ERROR; }
            if (state.cbInQue == size) {
                uint32_t offset = 0;
                while (offset != size) {
                    uint32_t actual = 0;
                    const EJODIN4_Status result = transfer(true, bytes + offset, size - offset,
                        actual, deadline, cancel, error);
                    if (result != EJODIN4_OK) return result;
                    if (!actual || actual > size - offset) { error = "Incomplete queued Samsung frame"; return EJODIN4_USB_IO_ERROR; }
                    offset += actual;
                }
                present = true;
                return EJODIN4_OK;
            }
            const uint64_t now = GetTickCount64();
            if (now >= deadline) {
                if (state.cbInQue) { error = "Partial optional Samsung frame remains queued"; return EJODIN4_USB_IO_ERROR; }
                return EJODIN4_OK;
            }
            // No read is outstanding while waiting. A partial frame remains
            // queued on timeout, so cancellation cannot turn consumed data into
            // a false 'absent' result through the ordinary pending-I/O drain.
            Sleep(static_cast<DWORD>(std::min<uint64_t>(5, deadline - now)));
        }
    }
    EJODIN4_Status finish(std::string& error) {
        if (restore_ && !SetCommTimeouts(file_, &original_)) {
            error = serial_error("Restore Samsung communication timeouts");
            return EJODIN4_USB_IO_ERROR;
        }
        restore_ = false;
        return EJODIN4_OK;
    }
};
}
EJODIN4_Status open_native_serial(const EJODIN4_UsbDevice& device,
    std::unique_ptr<SerialConnection>& connection, uint64_t deadline,
    const Cancellation& cancel, std::string& error) {
    std::unique_ptr<NativeSerial> native(new NativeSerial);
    const EJODIN4_Status status = native->open(device, deadline, cancel, error);
    if (status == EJODIN4_OK) connection.reset(native.release());
    return status;
}
}

namespace {
EJODIN4_Status download_result(EJODIN4_Status status, const char* error, char* diagnostic, uint32_t capacity) {
    if (diagnostic && capacity) {
        const size_t size = std::min<size_t>(std::strlen(error), capacity - 1);
        std::memcpy(diagnostic, error, size); diagnostic[size] = 0;
    }
    return status;
}
}
EJODIN4_Status EJODIN4_CALL EJODIN4_EnumerateDownloadDevices(
    EJODIN4_UsbDevice* devices, uint32_t capacity, uint32_t* count,
    EJODIN4_CancelCallback cancel, void* context, char* diagnostic, uint32_t diagnostic_capacity) {
    if (count) *count = 0;
    try {
        if ((!diagnostic && diagnostic_capacity) || !count || (!devices && capacity))
            return download_result(EJODIN4_INVALID_ARGUMENT, "Invalid Download enumeration arguments", diagnostic, diagnostic_capacity);
        const ejodin4::Cancellation cancellation = {cancel, context};
        if (cancellation.requested()) return download_result(EJODIN4_CANCELLED, "Download enumeration cancelled", diagnostic, diagnostic_capacity);
        std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
        std::string error;
        const EJODIN4_Status status = ejodin4::enumerate(*platform, devices, capacity, count, cancellation, error);
        return download_result(status, error.c_str(), diagnostic, diagnostic_capacity);
    } catch (const std::exception& error) {
        if (count) *count = 0;
        return download_result(EJODIN4_INTERNAL_ERROR, error.what(), diagnostic, diagnostic_capacity);
    } catch (...) {
        if (count) *count = 0;
        return download_result(EJODIN4_INTERNAL_ERROR, "Download enumeration raised an exception", diagnostic, diagnostic_capacity);
    }
}
EJODIN4_Status EJODIN4_CALL EJODIN4_ProbeDownloadConnection(
    const char* path, uint32_t bytes, uint32_t timeout, EJODIN4_DownloadProbe* output,
    EJODIN4_CancelCallback cancel, void* context, char* diagnostic, uint32_t diagnostic_capacity) {
    try {
        if ((!diagnostic && diagnostic_capacity) || !output || !ejodin4::valid_utf8_path(path, bytes) || !timeout || timeout > 30000)
            return download_result(EJODIN4_INVALID_ARGUMENT, "Invalid Download probe arguments", diagnostic, diagnostic_capacity);
        const ejodin4::Cancellation cancellation = {cancel, context};
        if (cancellation.requested()) return download_result(EJODIN4_CANCELLED, "Download probe cancelled", diagnostic, diagnostic_capacity);
        std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
        std::string error;
        const EJODIN4_Status status = ejodin4::probe_download(*platform, path, bytes, timeout, output, cancellation, error);
        return download_result(status, error.c_str(), diagnostic, diagnostic_capacity);
    } catch (const std::exception& error) {
        return download_result(EJODIN4_INTERNAL_ERROR, error.what(), diagnostic, diagnostic_capacity);
    } catch (...) {
        return download_result(EJODIN4_INTERNAL_ERROR, "Download probe raised an exception", diagnostic, diagnostic_capacity);
    }
}
