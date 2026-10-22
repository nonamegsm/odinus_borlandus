// Hardware-free production TCP transport checks. Only IPv4 loopback is used.
#include <winsock2.h>
#include <windows.h>
#include "Odin4Tcp.h"
#include "Odin4DownloadChannel.h"
#include "Odin4Session.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
const ejodin4::Cancellation never = {0, 0};
class Socket {
    SOCKET value_;
    Socket(const Socket&);
    Socket& operator=(const Socket&);
public:
    explicit Socket(SOCKET value = INVALID_SOCKET) : value_(value) {}
    ~Socket() { if (value_ != INVALID_SOCKET) closesocket(value_); }
    SOCKET get() const { return value_; }
    void reset(SOCKET value = INVALID_SOCKET) {
        if (value_ != INVALID_SOCKET) closesocket(value_);
        value_ = value;
    }
};
bool nonblocking(SOCKET socket) {
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}
std::string endpoint(const char* scheme, unsigned port) {
    char value[96];
    std::sprintf(value, "%s127.0.0.1:%u", scheme, port);
    return value;
}
unsigned bind_loopback(Socket& socket, bool listening) {
    socket.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    check(socket.get() != INVALID_SOCKET, "create loopback fixture socket");
    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    check(bind(socket.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
        "reserve OS-assigned loopback port");
    int bytes = sizeof(address);
    check(getsockname(socket.get(), reinterpret_cast<sockaddr*>(&address), &bytes) == 0,
        "read OS-assigned loopback port");
    if (listening) check(listen(socket.get(), 1) == 0, "listen fixture socket");
    check(nonblocking(socket.get()), "fixture socket uses bounded nonblocking I/O");
    return ntohs(address.sin_port);
}
// Every peer operation polls this stop event, and has an independent hard limit.
// Test assertions can unwind without leaving a blocked peer thread behind.
enum PeerReply { FRAGMENTED_METADATA, SIMPLE_REPLY, SILENT_REPLY, CLOSED_REPLY, BAD_REPLY };
struct Peer {
    typedef void (*Script)(Peer&, SOCKET, void*);
    Socket listener;
    unsigned port;
    bool outbound;
    PeerReply reply;
    HANDLE stop, thread;
    bool ok;
    std::string failure;
    DWORD started;
    Script script;
    void* script_context;
    Peer(bool production_outbound, PeerReply response, unsigned same_port = 0,
        Script session_script = 0, void* context = 0)
        : port(same_port), outbound(production_outbound), reply(response), stop(0), thread(0), ok(false), started(0),
          script(session_script), script_context(context) {
        if (outbound) port = bind_loopback(listener, true);
        else if (!port) { Socket reservation; port = bind_loopback(reservation, false); }
        stop = CreateEvent(0, TRUE, FALSE, 0);
        check(stop != 0, "create fixture stop event");
        thread = CreateThread(0, 0, run, this, 0, 0);
        if (!thread) { CloseHandle(stop); stop = 0; check(false, "create loopback peer thread"); }
    }
    ~Peer() {
        if (stop) SetEvent(stop);
        if (thread) { WaitForSingleObject(thread, 6000); CloseHandle(thread); }
        if (stop) CloseHandle(stop);
    }
    bool active() const {
        return WaitForSingleObject(stop, 0) == WAIT_TIMEOUT && DWORD(GetTickCount() - started) < 5000;
    }
    bool ready(SOCKET socket, bool read) {
        while (active()) {
            fd_set set;
            FD_ZERO(&set); FD_SET(socket, &set);
            timeval wait; wait.tv_sec = 0; wait.tv_usec = 20000;
            const int result = select(0, read ? &set : 0, read ? 0 : &set, 0, &wait);
            if (result > 0) return true;
            if (result == SOCKET_ERROR) return false;
        }
        return false;
    }
    bool bytes(SOCKET socket, bool read, char* data, unsigned count) {
        unsigned offset = 0;
        while (offset < count && active()) {
            if (!ready(socket, read)) return false;
            int actual = read ? recv(socket, data + offset, count - offset, 0) :
                send(socket, data + offset, count - offset, 0);
            if (actual > 0) offset += static_cast<unsigned>(actual);
            else if (actual == 0 || WSAGetLastError() != WSAEWOULDBLOCK) return false;
        }
        return offset == count;
    }
    bool connect_peer(Socket& connection) {
        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<u_short>(port));
        // The native listener is created by the main thread after this peer
        // starts. Retry refused connections until it is ready, with no fixed
        // startup delay or assumed TCP port.
        while (active()) {
            connection.reset(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            if (connection.get() == INVALID_SOCKET || !nonblocking(connection.get())) return false;
            int result = connect(connection.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
            if (result == 0) return true;
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAECONNREFUSED) return false;
            fd_set write_set, error_set;
            FD_ZERO(&write_set); FD_ZERO(&error_set);
            FD_SET(connection.get(), &write_set); FD_SET(connection.get(), &error_set);
            timeval wait; wait.tv_sec = 0; wait.tv_usec = 20000;
            result = select(0, 0, &write_set, &error_set, &wait);
            if (result > 0) {
                int socket_error = 0, size = sizeof(socket_error);
                if (getsockopt(connection.get(), SOL_SOCKET, SO_ERROR,
                    reinterpret_cast<char*>(&socket_error), &size) != 0) return false;
                if (!socket_error) return true;
                if (socket_error != WSAECONNREFUSED) return false;
            } else if (result == SOCKET_ERROR) return false;
            if (WaitForSingleObject(stop, 10) != WAIT_TIMEOUT) return false;
        }
        return false;
    }
    void serve() {
        started = GetTickCount();
        Socket connection;
        if (outbound) {
            if (!ready(listener.get(), true)) { failure = "fixture accept timed out"; return; }
            connection.reset(accept(listener.get(), 0, 0));
            if (connection.get() == INVALID_SOCKET || !nonblocking(connection.get())) {
                failure = "fixture accept failed"; return;
            }
        } else if (!connect_peer(connection)) { failure = "fixture connect failed"; return; }
        char hello[4];
        if (!bytes(connection.get(), true, hello, sizeof(hello)) || std::memcmp(hello, "ODIN", 4)) {
            failure = "native channel did not send exact ODIN handshake"; return;
        }
        if (script) { script(*this, connection.get(), script_context); ok = true; return; }
        if (reply == CLOSED_REPLY) { ok = true; return; }
        if (reply == FRAGMENTED_METADATA) {
            char first[] = {'L', 'O'};
            char following[] = {'K', 'E', 0x65, 0, 0, 0, 1, 0, 0, 0, 'N', 'E', 'X', 'T'};
            if (!bytes(connection.get(), false, first, sizeof(first))) { failure = "fixture first fragment failed"; return; }
            // Separate writes with a bounded pause exercise a short LOKE read;
            // the final fragment deliberately shares a write with later frames.
            if (WaitForSingleObject(stop, 30) != WAIT_TIMEOUT) return;
            if (!bytes(connection.get(), false, following, sizeof(following))) { failure = "fixture remaining fragment failed"; return; }
        } else if (reply != SILENT_REPLY) {
            char response[] = {'L', 'O', 'K', 'E'};
            if (reply == BAD_REPLY) response[0] = 'X';
            if (!bytes(connection.get(), false, response, sizeof(response))) { failure = "fixture LOKE write failed"; return; }
        }
        ok = true;
        while (active()) {
            if (WaitForSingleObject(stop, 20) != WAIT_TIMEOUT) return;
        }
    }
    static DWORD WINAPI run(void* context) {
        Peer& peer = *static_cast<Peer*>(context);
        try { peer.serve(); }
        catch (const std::exception& error) { peer.failure = error.what(); }
        catch (...) { peer.failure = "unexpected fixture exception"; }
        return 0;
    }
    void finish() {
        SetEvent(stop);
        check(WaitForSingleObject(thread, 6000) == WAIT_OBJECT_0, "loopback peer terminates within fixture deadline");
        check(ok && failure.empty(), failure.empty() ? "loopback peer completed" : failure.c_str());
    }
};
struct TimedCancel {
    DWORD started, delay;
    unsigned calls;
    TimedCancel() : started(GetTickCount()), delay(0x7fffffffu), calls(0) {}
};
int32_t EJODIN4_CALL cancel_after(void* context) {
    TimedCancel& value = *static_cast<TimedCancel*>(context);
    ++value.calls;
    return DWORD(GetTickCount() - value.started) >= value.delay ? 1 : 0;
}
void bounded_elapsed(DWORD started, const char* label) {
    const DWORD elapsed = GetTickCount() - started;
    check(elapsed >= 60 && elapsed < 2000, label);
}
void test_endpoints() {
    ejodin4::TcpEndpoint result;
    const char* good[] = {"tcp://127.0.0.1:1", "tcp://255.255.255.255:65535",
        "tcp-listen://0.0.0.0:12345", "tcp-listen://127.0.0.1:65535"};
    for (unsigned i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        check(ejodin4::parse_tcp_endpoint(good[i], static_cast<uint32_t>(std::strlen(good[i]) + 1), result),
            "accept exact numeric IPv4 endpoint");
        check(result.listen == (i >= 2) && !result.address.empty() && result.port != 0,
            "endpoint parser retains direction, address and port");
    }
    const char* bad[] = {"", "tcp://localhost:1234", "tcp://[::1]:1234", "tcp://127.0.0.1",
        "tcp://127.0.0.1:", "tcp://127.0.0.1:0", "tcp://127.0.0.1:65536", "tcp://127.0.0.1:-1",
        "tcp://127.0.0.1:+1", "tcp://127.0.0.1:1x", "tcp://127.0.0.1:1/", "tcp://127.0.0.1: 1",
        "tcp://127.0.0.1:1 ", "tcp://127.0.0.1:99999999999999999999", "tcp://256.0.0.1:1",
        "tcp://127.0.0:1", "tcp://127.0.0.1.1:1", "tcp://127.0.0.x:1", "tcp://:1234",
        "TCP://127.0.0.1:1234", "tcp-listen://127.0.0.1:0", "tcp://user@127.0.0.1:1234"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        check(!ejodin4::parse_tcp_endpoint(bad[i], static_cast<uint32_t>(std::strlen(bad[i]) + 1), result),
            "reject malformed, nonnumeric or out-of-range TCP endpoint");
    const char embedded[] = "tcp://127.0.0.1:1234\0garbage";
    check(!ejodin4::parse_tcp_endpoint(embedded, sizeof(embedded), result), "reject endpoint interior NUL");
    check(!ejodin4::parse_tcp_endpoint(good[0], static_cast<uint32_t>(std::strlen(good[0])), result),
        "reject endpoint without bounded terminator");
    check(!ejodin4::parse_tcp_endpoint(0, 0, result), "reject null endpoint");
}
void test_handshake(bool outbound, unsigned same_port = 0) {
    Peer peer(outbound, FRAGMENTED_METADATA, same_port);
    std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
    ejodin4::DownloadChannel channel(*platform, never);
    const std::string path = endpoint(outbound ? "tcp://" : "tcp-listen://", peer.port);
    std::string error;
    check(channel.open(path.c_str(), path.size() + 1, 2000, error) == EJODIN4_OK,
        outbound ? "native outbound TCP channel connects" : "native inbound TCP channel accepts");
    check(channel.opened() && channel.transport() == EJODIN4_DOWNLOAD_TCP,
        "TCP is reported as its own transport");
    check(channel.handshake(error) == EJODIN4_OK, "fragmented LOKE handshake succeeds");
    check(channel.usb_boundary(true, error) == EJODIN4_OK && channel.usb_boundary(false, error) == EJODIN4_OK,
        "TCP USB boundary calls do not submit socket transfers");
    uint8_t ack[8] = {0}; bool tail = true; uint32_t word = 0xffffffffu;
    check(channel.read_metadata_ack(ack, 1000, tail, word, error) == EJODIN4_OK &&
        ack[0] == 0x65 && ack[4] == 1 && !tail && !word,
        "TCP metadata consumes exactly eight bytes without an optional serial tail");
    uint32_t id = 77, status = 88; bool present = true;
    check(channel.optional_pit_ack(id, status, present, error) == EJODIN4_OK &&
        !present && id == 77 && status == 88, "TCP optional serial PIT acknowledgment does not consume bytes");
    uint8_t next[4] = {0};
    check(channel.read_exact(next, sizeof(next), 1000, error) == EJODIN4_OK && !std::memcmp(next, "NEXT", 4),
        "coalesced post-handshake frames remain queued and aligned");
    check(channel.finish(error) == EJODIN4_OK && !channel.opened(), "TCP teardown releases channel");
    peer.finish();
}
void test_waiting_listener(bool cancelled) {
    Socket reservation;
    const unsigned port = bind_loopback(reservation, false);
    reservation.reset();
    std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
    TimedCancel context; context.delay = cancelled ? 100 : 0x7fffffffu;
    ejodin4::Cancellation cancellation = {cancel_after, &context};
    ejodin4::DownloadChannel channel(*platform, cancellation);
    const std::string path = endpoint("tcp-listen://", port);
    std::string error; const DWORD started = GetTickCount();
    check(channel.open(path.c_str(), path.size() + 1, cancelled ? 3000 : 120, error) ==
        (cancelled ? EJODIN4_CANCELLED : EJODIN4_TIMEOUT),
        cancelled ? "cancel pending native TCP accept" : "native TCP accept obeys deadline");
    bounded_elapsed(started, "pending accept has bounded elapsed time");
    check(!channel.opened() && !error.empty() && context.calls > 1,
        "failed accept reports cause and leaves no open channel");
    // Immediately accepting on that exact port proves listener ownership was
    // released both on cancellation and deadline expiry.
    test_handshake(false, port);
}
void test_receive(PeerReply reply, bool cancelled) {
    Peer peer(true, reply);
    std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
    TimedCancel context;
    ejodin4::Cancellation cancellation = {cancel_after, &context};
    ejodin4::DownloadChannel channel(*platform, cancellation);
    const std::string path = endpoint("tcp://", peer.port);
    std::string error;
    check(channel.open(path.c_str(), path.size() + 1, 1000, error) == EJODIN4_OK,
        "open production TCP channel for receive failure test");
    if (reply == SILENT_REPLY) {
        // A short handshake deadline also covers the exact-read loop rather
        // than relying on a fake transfer returning a timeout.
        context.started = GetTickCount(); context.delay = cancelled ? 100 : 0x7fffffffu;
        const DWORD started = GetTickCount();
        const EJODIN4_Status status = channel.handshake(error);
        check(status == (cancelled ? EJODIN4_CANCELLED : EJODIN4_TIMEOUT),
            cancelled ? "cancel a pending native TCP receive" : "native TCP receive obeys deadline");
        bounded_elapsed(started, "pending receive has bounded elapsed time");
    } else {
        const EJODIN4_Status expected = reply == CLOSED_REPLY ? EJODIN4_USB_IO_ERROR : EJODIN4_BAD_HANDSHAKE;
        check(channel.handshake(error) == expected, reply == CLOSED_REPLY ?
            "closed TCP peer reports I/O failure" : "invalid TCP handshake is rejected");
    }
    check(!error.empty(), "TCP receive failure includes diagnostic");
    check(channel.finish(error) == EJODIN4_OK, "failed TCP session tears down cleanly");
    peer.finish();
}
void test_listener_rebind() {
    Socket reservation;
    const unsigned port = bind_loopback(reservation, false);
    reservation.reset();
    test_handshake(false, port);
    test_handshake(false, port);
}
void test_public_probe(bool outbound, bool bad_handshake) {
    Peer peer(outbound, bad_handshake ? BAD_REPLY : FRAGMENTED_METADATA);
    const std::string path = endpoint(outbound ? "tcp://" : "tcp-listen://", peer.port);
    EJODIN4_DownloadProbe output;
    std::memset(&output, 0xa5, sizeof(output));
    const EJODIN4_DownloadProbe original = output;
    char diagnostic[512];
    std::memset(diagnostic, 0xa5, sizeof(diagnostic));
    const EJODIN4_Status status = EJODIN4_ProbeDownloadConnection(path.c_str(),
        static_cast<uint32_t>(path.size() + 1), 2000, &output, 0, 0,
        diagnostic, sizeof(diagnostic));
    if (bad_handshake) {
        check(status == EJODIN4_BAD_HANDSHAKE, "public TCP probe rejects non-LOKE handshake");
        check(!std::memcmp(&output, &original, sizeof(output)),
            "failed public TCP probe preserves all sentinel output bytes");
        check(diagnostic[0] && std::memchr(diagnostic, 0, sizeof(diagnostic)),
            "failed public TCP probe returns a bounded diagnostic");
    } else {
        check(status == EJODIN4_OK, outbound ?
            "public TCP probe connects to a loopback device" : "public TCP probe accepts a loopback device");
        check(!output.vendor_id && !output.product_id && output.transport == EJODIN4_DOWNLOAD_TCP &&
            output.handshake_complete == 1 && !output.reserved[0] && !output.reserved[1] && !output.reserved[2],
            "public TCP probe reports completed TCP handshake without invented USB identity");
        check(!diagnostic[0], "successful public TCP probe clears diagnostic");
    }
    peer.finish();
}
void test_public_probe_invalid() {
    const char* paths[] = {"tcp://127.0.0.1:0", "tcp://127.0.0.1:65536", "tcp://localhost:1234",
        "tcp://127.0.0.1:1234garbage", "tcp-listen://127.0.0.1:-1"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        EJODIN4_DownloadProbe output;
        std::memset(&output, 0xa5, sizeof(output));
        const EJODIN4_DownloadProbe original = output;
        char diagnostic[512] = {0};
        const DWORD started = GetTickCount();
        check(EJODIN4_ProbeDownloadConnection(paths[i], static_cast<uint32_t>(std::strlen(paths[i]) + 1),
            30000, &output, 0, 0, diagnostic, sizeof(diagnostic)) == EJODIN4_INVALID_ARGUMENT,
            "public TCP probe rejects malformed endpoint before connection or USB discovery");
        check(DWORD(GetTickCount() - started) < 1000, "malformed public TCP endpoint fails immediately");
        check(!std::memcmp(&output, &original, sizeof(output)) && diagnostic[0] &&
            std::memchr(diagnostic, 0, sizeof(diagnostic)),
            "malformed public TCP probe preserves output and reports bounded diagnostic");
    }
    const char embedded[] = "tcp://127.0.0.1:1234\0garbage";
    EJODIN4_DownloadProbe output;
    std::memset(&output, 0xa5, sizeof(output));
    const EJODIN4_DownloadProbe original = output;
    char diagnostic[512] = {0};
    check(EJODIN4_ProbeDownloadConnection(embedded, sizeof(embedded), 30000,
        &output, 0, 0, diagnostic, sizeof(diagnostic)) == EJODIN4_INVALID_ARGUMENT &&
        !std::memcmp(&output, &original, sizeof(output)),
        "public TCP probe rejects interior NUL without modifying output");
}
uint32_t word32(const uint8_t* value) {
    return uint32_t(value[0]) | uint32_t(value[1]) << 8 |
        uint32_t(value[2]) << 16 | uint32_t(value[3]) << 24;
}
void store32(uint8_t* value, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) value[i] = uint8_t(word >> (8 * i));
}
// Script assertions do not touch the main thread's check counter.
void peer_require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
enum RepartitionReply {
    PIT_SUCCESS, PIT_SUCCESS_65, PIT_OLD_LAYOUT, PIT_NEGATIVE_ACK,
    PIT_FOREIGN_ACK, PIT_SHORT_ACK, PIT_CONTROL_EMPTY_ID,
    PIT_DISCONNECT_UPLOADED, PIT_CANCEL_UPLOADED
};
struct RepartitionScript {
    RepartitionReply reply;
    std::vector<uint8_t> candidate;
    unsigned starts, uploads, read_blocks, images, finishes;
    bool cancelled;
    std::vector<ejodin4::SessionState> states;
    explicit RepartitionScript(RepartitionReply mode)
        : reply(mode), candidate(660, 0), starts(0), uploads(0), read_blocks(0),
          images(0), finishes(0), cancelled(false) {
        store32(&candidate[0], 0x12349876); store32(&candidate[4], 1);
        store32(&candidate[32], 2); // MMC device type.
        store32(&candidate[36], 7); // Candidate BOOT route differs from old id 1.
        store32(&candidate[52], 4096);
        std::memcpy(&candidate[64], "BOOT", 4);
        std::memcpy(&candidate[96], "boot.img", 8);
        candidate.back() = 0xa5; // Preserve raw padding bytes, too.
    }
    static void progress(void* context, const ejodin4::SessionState& state) {
        RepartitionScript& self = *static_cast<RepartitionScript*>(context);
        self.states.push_back(state);
        if (self.reply == PIT_CANCEL_UPLOADED && state.phase == EJODIN4_PHASE_PIT_UPLOAD_COMPLETE)
            self.cancelled = true;
    }
    static int32_t EJODIN4_CALL cancel(void* context) {
        return static_cast<RepartitionScript*>(context)->cancelled ? 1 : 0;
    }
    void control(Peer& peer, SOCKET socket, uint32_t cmd, uint32_t sub,
        const uint32_t* words = 0, unsigned count = 0) {
        uint8_t bytes[1024];
        peer_require(peer.bytes(socket, true, reinterpret_cast<char*>(bytes), sizeof(bytes)),
            "session peer receives complete 1024-byte control");
        peer_require(word32(bytes) == cmd && word32(bytes + 4) == sub,
            "session peer receives expected command in exact order without replay, metadata tail or USB framing");
        for (unsigned i = 0; i < count; ++i)
            peer_require(word32(bytes + 8 + 4 * i) == words[i], "session control has exact payload");
        for (unsigned i = 8 + 4 * count; i < sizeof(bytes); ++i)
            peer_require(bytes[i] == 0, "session control has zero padding");
        if (cmd == 0x65 && sub == 0) ++starts;
        if (cmd == 0x66 && sub == 0) ++images;
        if (cmd == 0x67 && sub == 0) ++finishes;
    }
    void ack(Peer& peer, SOCKET socket, uint32_t id, uint32_t status = 0, unsigned count = 8) {
        uint8_t bytes[8]; store32(bytes, id); store32(bytes + 4, status);
        peer_require(peer.bytes(socket, false, reinterpret_cast<char*>(bytes), count),
            "session peer sends bounded acknowledgement");
    }
    void closed_without_commands(Peer& peer, SOCKET socket) {
        peer_require(peer.ready(socket, true), "session closes within peer deadline");
        char byte;
        const int received = recv(socket, &byte, 1, 0);
        peer_require(received == 0 || (received == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET),
            "no image, finalization, retry or replay is sent after terminal PIT failure");
    }
    static void run(Peer& peer, SOCKET socket, void* context) {
        static_cast<RepartitionScript*>(context)->serve(peer, socket);
    }
    void serve(Peer& peer, SOCKET socket) {
        char hello[] = {'L', 'O', 'K', 'E'};
        peer_require(peer.bytes(socket, false, hello, 4), "session peer sends exact LOKE");
        const uint32_t protocol = 5, packet = 1048576, total[] = {513, 0};
        control(peer, socket, 0x64, 0, &protocol, 1); ack(peer, socket, 0x64, 2u << 16);
        control(peer, socket, 0x64, 5, &packet, 1); ack(peer, socket, 0x64);
        control(peer, socket, 0x64, 2, total, 2); ack(peer, socket, 0x64);
        control(peer, socket, 0x65, 0);
        if (reply == PIT_CONTROL_EMPTY_ID) {
            ack(peer, socket, 0); closed_without_commands(peer, socket); return;
        }
        ack(peer, socket, 0x65);
        const uint32_t size = uint32_t(candidate.size());
        control(peer, socket, 0x65, 2, &size, 1); ack(peer, socket, 0x65);
        std::vector<uint8_t> raw(candidate.size());
        peer_require(peer.bytes(socket, true, reinterpret_cast<char*>(&raw[0]), unsigned(raw.size())),
            "peer receives raw PIT exact declared byte count");
        peer_require(raw == candidate, "raw PIT is byte-identical and is not control-packet padded");
        ++uploads;
        if (reply == PIT_NEGATIVE_ACK || reply == PIT_FOREIGN_ACK || reply == PIT_SHORT_ACK) {
            ack(peer, socket, reply == PIT_FOREIGN_ACK ? 0x66 : 0,
                reply == PIT_NEGATIVE_ACK ? 0xffffffffu : 0, reply == PIT_SHORT_ACK ? 3 : 8);
            if (reply == PIT_SHORT_ACK) shutdown(socket, SD_SEND);
            closed_without_commands(peer, socket); return;
        }
        ack(peer, socket, reply == PIT_SUCCESS_65 ? 0x65 : 0);
        control(peer, socket, 0x65, 3, &size, 1); ack(peer, socket, 0x65);
        if (reply == PIT_DISCONNECT_UPLOADED) return;
        if (reply == PIT_CANCEL_UPLOADED) { closed_without_commands(peer, socket); return; }
        control(peer, socket, 0x65, 1); ack(peer, socket, 0x65, size);
        std::vector<uint8_t> readback = candidate;
        if (reply == PIT_OLD_LAYOUT) store32(&readback[36], 1);
        for (uint32_t offset = 0, index = 0; offset < size; offset += 500, ++index) {
            control(peer, socket, 0x65, 2, &index, 1);
            const uint32_t count = std::min<uint32_t>(500, size - offset);
            peer_require(peer.bytes(socket, false, reinterpret_cast<char*>(&readback[offset]), count),
                "read-back PIT sends exact 500-byte chunks and short final chunk without optional tail");
            ++read_blocks;
        }
        control(peer, socket, 0x65, 3); ack(peer, socket, 0x65);
        if (reply == PIT_OLD_LAYOUT) { closed_without_commands(peer, socket); return; }
        control(peer, socket, 0x66, 0); ack(peer, socket, 0x66);
        const uint32_t start = 131072;
        control(peer, socket, 0x66, 2, &start, 1); ack(peer, socket, 0x66);
        std::vector<uint8_t> image(packet);
        peer_require(peer.bytes(socket, true, reinterpret_cast<char*>(&image[0]), packet),
            "peer receives one negotiated image packet");
        for (unsigned i = 0; i < packet; ++i)
            peer_require(image[i] == (i < 513 ? 0x5a : 0), "image data and packet padding are exact");
        ack(peer, socket, 0);
        const uint32_t complete[] = {0, 513, 0, 2, 7, 1, 0, 0};
        control(peer, socket, 0x66, 3, complete, 8); ack(peer, socket, 0x66);
        control(peer, socket, 0x67, 0); ack(peer, socket, 0x67);
        closed_without_commands(peer, socket);
    }
};
class LoopbackImageReader : public ejodin4::ImageReader {
    uint32_t remaining_;
public:
    LoopbackImageReader() : remaining_(513) {}
    EJODIN4_Status read(uint8_t* output, uint32_t capacity, uint32_t& actual,
        const ejodin4::Cancellation&, std::string&) {
        actual = std::min(capacity, remaining_); remaining_ -= actual;
        std::memset(output, 0x5a, actual); return EJODIN4_OK;
    }
};
void test_repartition_session(bool outbound, RepartitionReply reply) {
    RepartitionScript script(reply);
    Peer peer(outbound, SIMPLE_REPLY, 0, RepartitionScript::run, &script);
    std::unique_ptr<ejodin4::Platform> platform = ejodin4::make_native_platform(true);
    const ejodin4::Cancellation cancellation = {RepartitionScript::cancel, &script};
    ejodin4::DownloadChannel channel(*platform, cancellation);
    std::string error;
    const std::string path = endpoint(outbound ? "tcp://" : "tcp-listen://", peer.port);
    check(channel.open(path.c_str(), uint32_t(path.size() + 1), 1500, error) == EJODIN4_OK,
        "real TCP repartition session opens without USB discovery");
    ejodin4::Session session(channel, ejodin4::SessionProgress(RepartitionScript::progress, &script), 500);
    ejodin4::FirmwareIdentity identity; identity.present = true; identity.model = "SM-TEST_BUILD";
    std::vector<EJODIN4_PitEntry> live;
    const EJODIN4_Status result = session.begin(live, error, 513, &identity, &script.candidate);
    const bool success = reply == PIT_SUCCESS || reply == PIT_SUCCESS_65;
    const bool uploaded = success || reply == PIT_OLD_LAYOUT || reply == PIT_DISCONNECT_UPLOADED || reply == PIT_CANCEL_UPLOADED;
    check(session.state().any_write && session.state().pit_write_started,
        "PIT intent is retained after real TCP control submission");
    check(session.state().pit_uploaded == uploaded && session.state().pit_verified == success,
        "TCP proof state distinguishes started, acknowledged upload and verified layout");
    ejodin4::PlanImage image; image.decoded_bytes = 513; image.route.device_type = 2; image.route.identifier = 7;
    LoopbackImageReader reader;
    if (success) {
        check(result == EJODIN4_OK && live.size() == 1 && live[0].identifier == 7 && session.pit_bytes() == script.candidate,
            "TCP uploads candidate and verifies exact read-back before exposing new BOOT route");
        check(session.send_image(image, reader, error) == EJODIN4_OK && session.finish(false, error) == EJODIN4_OK,
            "TCP sends image and finalizes after verified repartition");
        check(session.state().finalized && session.state().images_completed == 1 && session.state().acknowledged_bytes == 513,
            "successful TCP repartition preserves image byte and completion evidence");
    } else {
        const EJODIN4_Status expected = reply == PIT_OLD_LAYOUT ? EJODIN4_PIT_MAPPING_ERROR :
            reply == PIT_CANCEL_UPLOADED ? EJODIN4_CANCELLED :
            (reply == PIT_SHORT_ACK || reply == PIT_DISCONNECT_UPLOADED) ? EJODIN4_USB_IO_ERROR : EJODIN4_INVALID_RESPONSE;
        check(result == expected && !error.empty(), "TCP repartition terminal failure reports exact cause");
        check(session.send_image(image, reader, error) == EJODIN4_INVALID_ARGUMENT &&
            session.finish(false, error) == EJODIN4_INVALID_ARGUMENT && !session.state().finalized && !session.state().images_completed,
            "failed or cancelled TCP repartition blocks images and finalization without replay");
        check(session.close(error) == EJODIN4_OK, "failed real TCP repartition closes channel");
    }
    bool saw_started = false, saw_uploaded = false, saw_verified = false;
    for (size_t i = 0; i < script.states.size(); ++i) {
        const ejodin4::SessionState& state = script.states[i];
        if (state.pit_write_started) saw_started = true;
        if (state.pit_uploaded) saw_uploaded = true;
        if (state.pit_verified) saw_verified = true;
        check(!state.pit_verified || state.pit_uploaded, "verified progress requires acknowledged upload");
        check(!state.pit_uploaded || state.pit_write_started, "uploaded progress requires submitted PIT intent");
        check(state.phase != EJODIN4_PHASE_IMAGE_BEGIN || state.pit_verified,
            "no TCP image progress occurs before verified read-back");
    }
    check(saw_started && saw_uploaded == uploaded && saw_verified == success,
        "TCP callbacks preserve PIT proof transitions on success and failure");
    check(WaitForSingleObject(peer.thread, 6000) == WAIT_OBJECT_0, "scripted TCP peer terminates after channel closes");
    peer.finish();
    check(script.starts == 1 && script.uploads == (reply == PIT_CONTROL_EMPTY_ID ? 0u : 1u),
        "real TCP submits at most one PIT upload without automatic replay");
    check(script.images == (success ? 1u : 0u) && script.finishes == (success ? 1u : 0u),
        "peer sees image and finalize only after successful read-back");
    check(script.read_blocks == ((success || reply == PIT_OLD_LAYOUT) ? 2u : 0u),
        "real TCP PIT read-back uses two correctly indexed bounded chunks");
}
}
int main() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) { std::fprintf(stderr, "FAIL: fixture WSAStartup\n"); return 1; }
    int result = 0;
    try {
        test_endpoints(); test_handshake(true); test_handshake(false);
        test_waiting_listener(false); test_waiting_listener(true);
        test_receive(SILENT_REPLY, false); test_receive(SILENT_REPLY, true);
        test_receive(CLOSED_REPLY, false); test_receive(BAD_REPLY, false);
        test_listener_rebind();
        test_public_probe(true, false); test_public_probe(false, false);
        test_public_probe(true, true); test_public_probe(false, true);
        test_public_probe_invalid();
        for (unsigned direction = 0; direction < 2; ++direction)
            for (unsigned reply = PIT_SUCCESS; reply <= PIT_CANCEL_UPLOADED; ++reply)
                test_repartition_session(direction != 0, RepartitionReply(reply));
        std::printf("PASS native TCP loopback: %u checks; no USB or remote device I/O\n", checks);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native TCP after %u checks: %s\n", checks, error.what()); result = 1;
    }
    WSACleanup();
    return result;
}
