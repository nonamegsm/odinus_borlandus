# Samsung Download protocol versions

This describes the native engine built into OdinBCC.exe as of 2026-10-18. The version is negotiated with the device and reported in EJODIN4_FlashResult.protocol_version. It is separate from the Odin application release, the Odin4 source/module name and the public C ABI returned by EJODIN4_GetAbiVersion().

## Support matrix

| Negotiated version | Native engine support | Image packet size | Default image data/ACK timeout |
| --- | --- | --- | --- |
| 1 | Supported | 128 KiB (131,072 bytes) | 30 seconds |
| 2 | Supported | 1 MiB (1,048,576 bytes) | 120 seconds |
| 3 | Stock driver: raw or negotiated encoded transfer; WinUSB: raw | 1 MiB (1,048,576 bytes) | 120 seconds |
| 4 | Stock driver only; DEVINFO model and authenticated firmware identity required for writes | 1 MiB (1,048,576 bytes) | 120 seconds |
| 5 | Stock driver only; DEVINFO model/carrier and authenticated firmware identity required for writes | 1 MiB (1,048,576 bytes) | 120 seconds |
| 0 or any other decoded value | Rejected as unsupported | No image transfer | Not applicable |

These defaults come from [Session construction and negotiation](../engine/src/Odin4Session.cpp). An explicit session timeout override replaces the image timeout and command timeout arguments; the handshake uses the channel timeout. The modern 120-second default applies to image data, its ACKs and sequence completion, not every control request.

## Negotiation and rejection

The channel sends the four-byte ODIN handshake and requires LOKE. Session::begin then sends a 1,024-byte, zero-filled control request containing little-endian words: command 0x64, subcommand 0, and a transport-specific maximum/probe value. A normal acknowledgement contains two little-endian words: response ID and ACK.

| Transport | Negotiation request value | Version decoded from negotiation ACK |
| --- | --- | --- |
| Samsung stock driver / serial | 5 | (ACK >> 16) & 0xffff |
| WinUSB | 0x7fffffff | (ACK >> 16) & 0x7fff |

The stock-driver path accepts versions 1 through 5; WinUSB accepts 1 through 3. The stock-driver path preserves the entire unsigned high word, including values such as 0x8002; WinUSB masks the high capability bit. SessionState.negotiation_ack retains the original ACK separately from later replies. The low-word 0x8000 capability flag does not change the decoded version.

An unsupported version returns EJODIN4_DRIVER_UNSUPPORTED immediately after negotiation, before packet-size setup, transfer-total metadata, PIT requests or image writes. The failed session cannot subsequently finalize or reboot.

Versions 2 through 5 send command 0x64, subcommand 5, with the 1 MiB packet size. Version 1 keeps the initial 128 KiB packet size and skips this command.

## DEVINFO and firmware identity

Stock-driver versions 4/5 read DEVINFO using 0x69/0 for size, indexed 0x69/1 requests with 500-byte replies, and 0x69/2 for completion. Version 4 requires a model record; version 5 also requires a carrier record. Read-only device inspection does not require a firmware manifest.

Writes require one signed meta-data/*.mf manifest in each archive contributing flashed images. Two manufacturer RSA public keys authenticate those manifests; member digests and model/carrier policy come from the selected package. The manifest model is compared with live DEVINFO, without a compiled allowed-model list. Missing or invalid identity fails before image commands. Protocols 1–3 do not require this identity proof. See [native protocol compatibility](NATIVE_PROTOCOL_COMPATIBILITY.md) for signature verification, policy and evidence limits.

## Raw transfer behavior shared by stock-driver versions 1–5

For the stock-driver path, version changes packet size and its default timeout; it does not select a different sequence-begin or completion format. Each sequence sends file-begin (0x66/0), transfer-start (0x66/2), full negotiated data packets with indexed ACKs, and completion (0x66/3). Transfer-start rounds the decoded sequence length up to 128 KiB; data writes round up to the negotiated packet size and zero-pad the tail. Completion carries the exact decoded byte count. Progress counts only acknowledged image bytes, excluding padding.

Stock-driver sequence limits depend on the PIT route, independently of protocol version:

| PIT binary type | PIT device type | Maximum decoded bytes per sequence |
| --- | --- | --- |
| 0 | 1, 2 or 8 | 30 MiB (31,457,280 bytes) |
| 0 | 6 | 4 MiB (4,194,304 bytes) |
| 0 | 7 | 1 MiB (1,048,576 bytes) |
| Any other combination | Any | Rejected as unsupported |

No compiled model, firmware-release or known-image-hash allowlist determines eligibility. Newer writes still require the package/device identity checks above. Automatic partition capacity checks use the selected PIT storage type (MMC 512-byte units; UFS 4096-byte units), with strict explicit overrides. The composed operation currently supports those two storage types even though the session has additional raw route handlers. See [PIT routing evidence and limits](PIT_ROUTING.md).

The flash operation validates every planned route before the first image write. With a nonzero planned transfer total, the stock-driver session announces the decoded total before reading PIT; its two total words use base UINT32_MAX (0xffffffff), rather than the WinUSB low/high 32-bit encoding.

WinUSB retains a 30 MiB sequence limit, file-begin once per image, USB packet-boundary handling, and a counter query/reset (0x64/1) after PIT and after each completed image. Its large-partition completion length rounds up to 512 bytes. The query/reset may return an 8- or 12-byte metadata response; the optional trailing word is opaque and response framing must stay consistent within the session. These transport differences are implemented independently of the accepted version number. See [the session](../engine/src/Odin4Session.cpp) and [the download channel](../engine/src/Odin4DownloadChannel.cpp).

## Negotiated encoded transfer

The stock-driver path enables encoded transfer only for protocol >=3 with capability bit 0x8000. Eligible LZ4 records use 0x66/5 to begin, 0x66/6 with stored and decoded lengths, padded indexed packets, and 0x66/7 to complete. Completion retains the exact decoded length and PIT route. Progress credits decoded bytes after sequence completion, rather than treating acknowledged compressed bytes as partition bytes.

Unsupported LZ4 frame profiles select host decoding before image writes. A transfer failure never triggers a raw retry or replay after writes begin. WinUSB retains its raw flow.

## Verification and limits

[Session tests](../tests/native_session_tests.cpp) cover legacy negotiation, route limits, native_devinfo_versions(), stock_encoded_tests(), composed operations, metadata framing, ACK failures, timeouts and cancellation. [Identity tests](../tests/native_firmware_identity_tests.cpp) cover manifest parsing and cryptographic verification; [DEVINFO tests](../tests/native_device_info_tests.cpp) cover record bounds and dynamic model/carrier policy. These are offline tests, not physical-device validation of every accepted version or transport.

Run the existing native suite from the repository root:

    py tests/run_tests.py --native-only --configuration Debug

Add --build-app to rebuild the application first. See [test instructions](../tests/README.md), [verification evidence](VERIFICATION.md) and [native protocol compatibility](NATIVE_PROTOCOL_COMPATIBILITY.md). A genuine manufacturer-signed positive archive and physical protocol 4/5 flashing remain untested. The reported earlier successful physical flash applies to the EasyJTAG implementation; the native Borland synchronization has not been physically flashed.
