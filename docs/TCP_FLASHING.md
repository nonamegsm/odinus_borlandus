# TCP/IP Download-mode flashing

Select **TCP listen** in the Device row to use the incoming connection pattern in [brokkr-flash](https://github.com/Gabriel2392/brokkr-flash). The PC listens; the device connects to the PC. This requires a device or relay that actually supports the Samsung Download protocol over TCP. An ordinary Android Wi-Fi or ADB connection does not provide this protocol.

1. Prepare the firmware and offline plan as usual.
2. Enter `0.0.0.0:13579` to accept on all local IPv4 interfaces, or a specific local IPv4 address and port to restrict the listener.
3. Start **Flash selected firmware** and confirm the write. The listener starts after firmware preflight. Connect the device in its network Download mode to this PC's reachable IP and port when the status says it is waiting.
4. Allow incoming TCP on the chosen port in Windows Firewall if needed. The initial connection wait is 30 seconds and Cancel interrupts it. Firmware preflight can take longer and does not open the listener.

**Check link** performs only ODIN/LOKE. **Read device PIT** opens a read session without image writes. Each operation accepts or connects once and closes its connection on completion: reconnect the device for the next operation. No listener runs merely from selecting TCP, at application startup, or during an offline plan.

**TCP connect** is also available for a device or relay that already listens. Enter its numeric IPv4 address and port, for example `192.168.1.20:13579`. DNS names and IPv6 are not currently supported. Bind addresses in listen mode belong to this PC; connect addresses belong to the remote server. The API accepts `tcp-listen://IPv4:port` and `tcp://IPv4:port` through the existing device-path argument.

Use a trusted network: this Download byte stream has no additional TLS or peer authentication. One accepted connection owns the entire operation; errors, timeouts and cancellation never trigger a reconnect or replay of firmware bytes.

## Protocol and validation

The independently written Winsock transport uses the four-byte ODIN/LOKE handshake and aggregates partial reads/writes with absolute deadlines. TCP ACKs are eight bytes; USB zero-length boundaries and optional Samsung serial PIT/metadata frames are not sent or consumed. Native protocol versions 1–5 share the existing route limits, 128 KiB raw start alignment, exact decoded completion lengths, capability-selected compressed download, source integrity and live PIT checks. Protocol 4/5 writes retain manufacturer-authenticated identity and DEVINFO policy checks.

Re-partition / write PIT is available in both TCP modes with complete clean BL/AP/CP/CSC firmware. Enable the existing checkbox, optionally choose a raw `.pit`, and prepare an offline plan before flashing. The engine validates the candidate before connecting, uploads it, then requires matching read-back before any firmware image. Ordinary writes and PIT reference archives remain metadata-only. WinUSB repartition remains unsupported. TCP errors remain failures even when a separate reboot reply is missing; the existing serial-specific completed-flash warning does not extend to TCP.

Reference inspected: brokkr-flash commit `2118902599857fc22cc559e0cdf73b2357d94fcc`, particularly `src/app/cli_mode.cpp`, `src/protocol/odin/odin_cmd.cpp`, and `src/protocol/odin/group_flasher.cpp`. No Brokkr implementation source or dependencies are included in this application.

## Verification and remaining gaps

`tests/native_tcp_tests.cpp` exercises the real Windows socket transport on IPv4 loopback: listen/connect, public connection probe, fragmented handshake, coalesced frames, exact ACK boundaries, disconnect, cancellation, timeouts and port release. `tests/native_session_tests.cpp` covers TCP session negotiation, raw and compressed image transfers, identity rejection, failed ACKs and full repartition operations through an injected device. Real loopback session tests additionally cover exact raw PIT upload, generic/PIT data ACKs, matching and mismatched read-back, malformed replies and interrupted sessions. These are hardware-free tests; physical TCP flashing/repartition and manufacturer-signed protocol-4/5 interoperability remain unverified.

Other capabilities identified in the comparison, outside this TCP change: decoded bootloader error messages, firmware drag-and-drop, reboot into Download mode, CLI automation, and coordinated multi-device flashing. Existing local LZ4, compressed download and PIT validation are already implemented.
