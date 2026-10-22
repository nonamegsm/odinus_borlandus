// winsock2 must precede every Windows header, including private USB headers.
#include <winsock2.h>
#include "Odin4Tcp.h"
#include "Odin4WinUsbApi.h"
#include <climits>
#include <cstring>
#include <sstream>

namespace ejodin4 {
bool parse_tcp_endpoint(const char* path, uint32_t bytes, TcpEndpoint& endpoint) {
    if (!valid_utf8_path(path, bytes)) return false;
    const std::string value(path, bytes - 1);
    TcpEndpoint parsed;
    size_t position = 0;
    if (value.compare(0, 6, "tcp://") == 0) position = 6;
    else if (value.compare(0, 13, "tcp-listen://") == 0) { position = 13; parsed.listen = true; }
    else return false;
    const size_t address_start = position;
    for (unsigned octet = 0; octet != 4; ++octet) {
        const size_t start = position;
        unsigned number = 0;
        while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
            number = number * 10 + static_cast<unsigned>(value[position++] - '0');
            if (number > 255 || position - start > 3) return false;
        }
        if (position == start || (position - start > 1 && value[start] == '0')) return false;
        if (position >= value.size() || value[position] != (octet == 3 ? ':' : '.')) return false;
        ++position;
    }
    parsed.address = value.substr(address_start, position - address_start - 1);
    const size_t port_start = position;
    unsigned port = 0;
    while (position < value.size() && value[position] >= '0' && value[position] <= '9') {
        port = port * 10 + static_cast<unsigned>(value[position++] - '0');
        if (port > 65535 || position - port_start > 5) return false;
    }
    if (position != value.size() || position == port_start || !port ||
        (position - port_start > 1 && value[port_start] == '0')) return false;
    if (!parsed.listen && parsed.address == "0.0.0.0") return false;
    parsed.port = static_cast<uint16_t>(port);
    endpoint = parsed;
    return true;
}

namespace {
std::string socket_error(const char* action, int code = WSAGetLastError()) {
    std::ostringstream message;
    message << action << " (Winsock error " << code << ")";
    return message.str();
}
EJODIN4_Status check_deadline(uint64_t deadline, const Cancellation& cancel, std::string& error) {
    if (cancel.requested()) { error = "TCP operation cancelled"; return EJODIN4_CANCELLED; }
    if (GetTickCount64() >= deadline) { error = "TCP operation timed out"; return EJODIN4_TIMEOUT; }
    return EJODIN4_OK;
}
EJODIN4_Status ready(SOCKET socket, bool read, uint64_t deadline,
    const Cancellation& cancel, std::string& error) {
    for (;;) {
        const EJODIN4_Status status = check_deadline(deadline, cancel, error);
        if (status != EJODIN4_OK) return status;
        const uint64_t now = GetTickCount64();
        if (now >= deadline) { error = "TCP operation timed out"; return EJODIN4_TIMEOUT; }
        const uint64_t remaining = deadline - now;
        const unsigned wait_ms = remaining > 50 ? 50 : static_cast<unsigned>(remaining);
        fd_set read_set, write_set, error_set;
        FD_ZERO(&read_set); FD_ZERO(&write_set); FD_ZERO(&error_set);
        if (read) FD_SET(socket, &read_set); else FD_SET(socket, &write_set);
        FD_SET(socket, &error_set);
        timeval timeout;
        timeout.tv_sec = 0; timeout.tv_usec = static_cast<long>(wait_ms * 1000);
        const int selected = select(0, &read_set, &write_set, &error_set, &timeout);
        if (selected == SOCKET_ERROR) { error = socket_error("Wait for TCP socket"); return EJODIN4_USB_IO_ERROR; }
        if (!selected) continue;
        if (FD_ISSET(socket, &error_set)) {
            int code = 0, length = sizeof(code);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&code), &length) == SOCKET_ERROR)
                code = WSAGetLastError();
            error = socket_error("TCP socket failed", code);
            return EJODIN4_USB_IO_ERROR;
        }
        return check_deadline(deadline, cancel, error);
    }
}
bool nonblocking(SOCKET socket, std::string& error) {
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) == SOCKET_ERROR) {
        error = socket_error("Set TCP socket nonblocking"); return false;
    }
    return true;
}
class NativeTcp : public SerialConnection {
    SOCKET socket_;
    bool winsock_;
    NativeTcp(const NativeTcp&);
    NativeTcp& operator=(const NativeTcp&);
    void close_socket() {
        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
    }
public:
    NativeTcp() : socket_(INVALID_SOCKET), winsock_(false) {}
    ~NativeTcp() {
        close_socket();
        if (winsock_) WSACleanup();
    }
    EJODIN4_Status open(const TcpEndpoint& endpoint, uint64_t deadline,
        const Cancellation& cancel, std::string& error) {
        EJODIN4_Status status = check_deadline(deadline, cancel, error);
        if (status != EJODIN4_OK) return status;
        WSADATA data;
        const int startup = WSAStartup(MAKEWORD(2, 2), &data);
        if (startup) { error = socket_error("Initialize TCP transport", startup); return EJODIN4_USB_IO_ERROR; }
        winsock_ = true;
        socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) { error = socket_error("Create TCP socket"); return EJODIN4_USB_IO_ERROR; }
        if (!nonblocking(socket_, error)) return EJODIN4_USB_IO_ERROR;
        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET; address.sin_port = htons(endpoint.port);
        unsigned a = 0, b = 0, c = 0, d = 0;
        // The strict parser has already bounded every dotted-decimal octet.
        std::istringstream input(endpoint.address);
        char dot;
        input >> a >> dot >> b >> dot >> c >> dot >> d;
        address.sin_addr.s_addr = htonl((a << 24) | (b << 16) | (c << 8) | d);
        if (endpoint.listen) {
            const BOOL exclusive = TRUE;
            if (setsockopt(socket_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR) {
                error = socket_error("Reserve TCP listen address"); return EJODIN4_USB_IO_ERROR;
            }
            if (bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
                error = socket_error("Bind TCP listener"); return EJODIN4_USB_IO_ERROR;
            }
            if (listen(socket_, 1) == SOCKET_ERROR) { error = socket_error("Listen for TCP connection"); return EJODIN4_USB_IO_ERROR; }
            for (;;) {
                status = ready(socket_, true, deadline, cancel, error);
                if (status != EJODIN4_OK) return status;
                const SOCKET accepted = accept(socket_, 0, 0);
                if (accepted != INVALID_SOCKET) {
                    close_socket(); socket_ = accepted;
                    if (!nonblocking(socket_, error)) return EJODIN4_USB_IO_ERROR;
                    break;
                }
                const int code = WSAGetLastError();
                if (code != WSAEWOULDBLOCK) { error = socket_error("Accept TCP connection", code); return EJODIN4_USB_IO_ERROR; }
            }
        } else if (connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            const int code = WSAGetLastError();
            if (code != WSAEWOULDBLOCK && code != WSAEINPROGRESS) {
                error = socket_error("Connect TCP endpoint", code); return EJODIN4_USB_IO_ERROR;
            }
            status = ready(socket_, false, deadline, cancel, error);
            if (status != EJODIN4_OK) return status;
            int connect_error = 0, length = sizeof(connect_error);
            if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connect_error), &length) == SOCKET_ERROR) {
                error = socket_error("Read TCP connect status"); return EJODIN4_USB_IO_ERROR;
            }
            if (connect_error) { error = socket_error("Connect TCP endpoint", connect_error); return EJODIN4_USB_IO_ERROR; }
        }
        const BOOL no_delay = TRUE;
        if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&no_delay), sizeof(no_delay)) == SOCKET_ERROR) {
            error = socket_error("Disable TCP packet delay"); return EJODIN4_USB_IO_ERROR;
        }
        return check_deadline(deadline, cancel, error);
    }
    EJODIN4_Status transfer(bool read, uint8_t* bytes, uint32_t capacity,
        uint32_t& actual, uint64_t deadline, const Cancellation& cancel, std::string& error) {
        actual = 0;
        if (socket_ == INVALID_SOCKET || !bytes || !capacity) {
            error = "TCP byte stream is closed or transfer is invalid"; return EJODIN4_USB_IO_ERROR;
        }
        for (;;) {
            const EJODIN4_Status status = ready(socket_, read, deadline, cancel, error);
            if (status != EJODIN4_OK) { close_socket(); return status; }
            const int length = capacity > static_cast<uint32_t>(INT_MAX) ? INT_MAX : static_cast<int>(capacity);
            const int count = read ? recv(socket_, reinterpret_cast<char*>(bytes), length, 0) :
                send(socket_, reinterpret_cast<const char*>(bytes), length, 0);
            if (count > 0) { actual = static_cast<uint32_t>(count); return EJODIN4_OK; }
            if (!count) { error = "TCP peer disconnected"; close_socket(); return EJODIN4_USB_IO_ERROR; }
            const int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK) continue;
            error = socket_error(read ? "Receive TCP bytes" : "Send TCP bytes", code);
            close_socket(); return EJODIN4_USB_IO_ERROR;
        }
    }
    EJODIN4_Status finish(std::string&) { close_socket(); return EJODIN4_OK; }
};
}
EJODIN4_Status open_native_tcp(const char* path,
    std::unique_ptr<SerialConnection>& connection, uint64_t deadline,
    const Cancellation& cancel, std::string& error) {
    TcpEndpoint endpoint;
    if (!path || !parse_tcp_endpoint(path, static_cast<uint32_t>(std::strlen(path) + 1), endpoint)) {
        error = "TCP endpoint must be tcp://IPv4:port or tcp-listen://IPv4:port";
        return EJODIN4_INVALID_ARGUMENT;
    }
    std::unique_ptr<NativeTcp> native(new NativeTcp);
    const EJODIN4_Status status = native->open(endpoint, deadline, cancel, error);
    if (status == EJODIN4_OK) connection.reset(native.release());
    return status;
}
}
