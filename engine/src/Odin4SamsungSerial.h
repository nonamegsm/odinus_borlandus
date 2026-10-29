#ifndef EASYJTAG_ODIN4_SAMSUNG_SERIAL_H
#define EASYJTAG_ODIN4_SAMSUNG_SERIAL_H
#include "Odin4UsbTransport.h"
namespace ejodin4 {
class SerialConnection {
public:
    virtual ~SerialConnection() {}
    virtual EJODIN4_Status transfer(bool read, uint8_t* bytes, uint32_t capacity,
        uint32_t& actual, uint64_t deadline, const Cancellation& cancel,
        std::string& error) = 0;
    virtual EJODIN4_Status finish(std::string& error) = 0;
    // A bounded optional 4/8-byte frame must not submit a cancellable read until
    // the full frame is queued. Default preserves existing probe-only fakes.
    virtual EJODIN4_Status read_optional_frame(uint8_t*, uint32_t, bool&, uint64_t,
        const Cancellation&, std::string& error) {
        error = "Optional Samsung serial frame inspection is unavailable";
        return EJODIN4_DRIVER_UNSUPPORTED;
    }
};
struct SerialTimeoutPolicy { uint32_t read_first_byte_ms,write_total_ms; };
bool serial_timeout_policy(uint64_t now,uint64_t deadline,SerialTimeoutPolicy& policy);
// Used only on a freshly opened exclusive handle, before protocol I/O.
class SerialStartup {
public:
    virtual ~SerialStartup() {}
    virtual bool set_mask(uint32_t mask,std::string& error)=0;
    virtual bool setup_buffers(uint32_t input,uint32_t output,std::string& error)=0;
    virtual bool purge(uint32_t flags,std::string& error)=0;
    virtual EJODIN4_Status set_timeouts(uint64_t deadline,std::string& error)=0;
    virtual uint64_t now() const=0;
    virtual void wait(uint32_t milliseconds)=0;
};
EJODIN4_Status initialize_serial(SerialStartup& startup,uint64_t deadline,
    const Cancellation& cancel,std::string& error);
bool valid_com_port(const std::wstring& port);
EJODIN4_Status probe_download(Platform& platform, const char* path, uint32_t bytes,
    uint32_t timeout, EJODIN4_DownloadProbe* output, const Cancellation& cancel,
    std::string& error);
EJODIN4_Status open_native_serial(const EJODIN4_UsbDevice& device,
    std::unique_ptr<SerialConnection>& connection, uint64_t deadline,
    const Cancellation& cancel, std::string& error);
}
#endif
