#ifndef EASYJTAG_ODIN4_DOWNLOAD_CHANNEL_H
#define EASYJTAG_ODIN4_DOWNLOAD_CHANNEL_H
#include "Odin4SamsungSerial.h"
namespace ejodin4 {
// Owns one selected interface from handshake until explicit cleanup. Never retries.
class DownloadChannel {
    Platform& platform_;
    Cancellation cancel_;
    EJODIN4_UsbDevice identity_;
    Selection pipes_;
    std::unique_ptr<Connection> usb_;
    std::unique_ptr<SerialConnection> serial_;
    std::unique_ptr<SerialConnection> tcp_;
    uint32_t timeout_;
    bool opened_;
    EJODIN4_Status exact(bool read, uint8_t*, uint32_t, uint32_t, std::string&);
public:
    DownloadChannel(Platform& platform, Cancellation cancel);
    ~DownloadChannel();
    DownloadChannel(const DownloadChannel&);
    DownloadChannel& operator=(const DownloadChannel&);
    EJODIN4_Status open(const char* path, uint32_t path_bytes, uint32_t timeout_ms, std::string& error);
    EJODIN4_Status handshake(std::string& error);
    EJODIN4_Status read_exact(uint8_t*, uint32_t, uint32_t timeout_ms, std::string&);
    EJODIN4_Status write_exact(const uint8_t*, uint32_t, uint32_t timeout_ms, std::string&);
    // Only serial has an optional eight-byte PIT completion frame. Timeout with
    // zero bytes means absent; any partial/foreign frame fails the session.
    EJODIN4_Status optional_pit_ack(uint32_t& id, uint32_t& ack, bool& present, std::string&);
    EJODIN4_Status read_metadata_ack(uint8_t* ack, uint32_t timeout,
        bool& tail_present, uint32_t& tail_word, std::string&);
    EJODIN4_Status usb_boundary(bool read, std::string& error);
    EJODIN4_Status finish(std::string& error);
    EJODIN4_Status interrupted(std::string& error) const;
    const EJODIN4_UsbDevice& identity() const { return identity_; }
    uint32_t transport() const { return identity_.flags; }
    uint32_t timeout() const { return timeout_; }
    bool opened() const { return opened_; }
    const Cancellation& cancellation() const { return cancel_; }
};
}
#endif
