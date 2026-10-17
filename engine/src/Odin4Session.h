#ifndef EASYJTAG_ODIN4_SESSION_H
#define EASYJTAG_ODIN4_SESSION_H
#include "Odin4DownloadChannel.h"
#include "Odin4FirmwareStream.h"

namespace ejodin4 {
struct SessionState {
    uint32_t protocol_version, transport, phase;
    uint32_t negotiation_ack;
    uint32_t images_completed, last_response_id, last_ack;
    uint64_t acknowledged_bytes, image_acknowledged_bytes;
    bool any_write, finalized, reboot_sent;
    bool metadata_tail_present;
    uint32_t metadata_tail_word;
    SessionState():protocol_version(0),transport(0),phase(0),negotiation_ack(0),images_completed(0),last_response_id(0),last_ack(0),acknowledged_bytes(0),image_acknowledged_bytes(0),any_write(false),finalized(false),reboot_sent(false),metadata_tail_present(false),metadata_tail_word(0){}
};
// Callback runs synchronously after real state changes; it must not throw.
typedef bcc::Callback1<const SessionState&> SessionProgress;
class Session {
    DownloadChannel& channel_;
    SessionProgress progress_;
    SessionState state_;
    uint32_t packet_bytes_, flash_timeout_;
    uint32_t timeout_override_;
    bool begun_, failed_, total_set_;
    bool metadata_baseline_set_, metadata_baseline_tail_;
    uint64_t total_;
    std::vector<uint8_t> pit_bytes_;
    EJODIN4_Status fail(EJODIN4_Status, const char*, std::string&);
    void phase(uint32_t);
    EJODIN4_Status announce_total(uint64_t, std::string&);
    EJODIN4_Status response(uint32_t expected, bool capability, uint32_t timeout, std::string&, bool metadata = false);
    EJODIN4_Status command(uint32_t command, uint32_t sub, const uint32_t* words,
        uint32_t word_count, bool capability, uint32_t timeout, std::string&);
public:
    explicit Session(DownloadChannel&, SessionProgress progress = SessionProgress(), uint32_t timeout_override_ms = 0);
    EJODIN4_Status begin(std::vector<EJODIN4_PitEntry>& live_pit, std::string& error, uint64_t planned_total = 0);
    EJODIN4_Status notify_total(uint64_t total, std::string& error);
    EJODIN4_Status validate_image(const PlanImage&, std::string&);
    EJODIN4_Status send_image(const PlanImage&, ImageReader&, std::string& error);
    EJODIN4_Status finish(bool reboot, std::string& error);
    EJODIN4_Status close(std::string& error);
    const SessionState& state() const { return state_; }
    const std::vector<uint8_t>& pit_bytes() const { return pit_bytes_; }
};
}
#endif
