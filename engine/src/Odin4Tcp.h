#ifndef EASYJTAG_ODIN4_TCP_H
#define EASYJTAG_ODIN4_TCP_H
#include "Odin4SamsungSerial.h"
namespace ejodin4 {
struct TcpEndpoint {
    bool listen;
    std::string address;
    uint16_t port;
    TcpEndpoint() : listen(false), port(0) {}
};
// Byte length includes the terminating NUL, as for other interface paths.
// Numeric dotted-decimal IPv4 only; no names, credentials, query or suffix.
bool parse_tcp_endpoint(const char* path, uint32_t bytes, TcpEndpoint& endpoint);
EJODIN4_Status open_native_tcp(const char* path,
    std::unique_ptr<SerialConnection>& connection, uint64_t deadline,
    const Cancellation& cancel, std::string& error);
}
#endif
