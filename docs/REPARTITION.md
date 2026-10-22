# Explicit repartition with firmware

ABI 1.12 adds **Re-partition / write PIT** through the Samsung stock modem driver or either TCP mode. It performs the native PIT upload sequence identified in the [REA audit](REPARTITION_AUDIT.md), reads the resulting PIT back, verifies the layout, then transfers the prepared firmware images. Ordinary flashing and the PIT reference archive never upload a PIT.

## Using the client

Select the complete BL, AP, CP and CSC archives. Enable **Re-partition / write PIT**; the client selects **Clean installation**. HOME_CSC, selected-slot updates and preserve-data policy cannot authorize repartitioning. The existing write confirmation identifies the device and PIT source and warns about partition-table replacement and data loss.

The separate **Raw PIT (optional)** field accepts a binary `.pit` file. With no raw file, the candidate comes from the PIT embedded in the selected firmware archives. A raw candidate explicitly overrides differing valid embedded PIT layouts; malformed embedded PITs still fail preflight. The PIT reference archive remains metadata-only and cannot be used to authorize this operation. XML conversion is not supported.

Prepare an offline plan before writing. Repartition plans validate the same complete clean firmware set, routes and capacities without opening a device, whether the PIT is embedded or supplied raw. No standalone PIT-only write or partial-firmware repartition operation is exposed.

## Validation and transfer

Before connecting, the engine snapshots a raw PIT into bounded immutable memory, validates PIT structure, maps every selected image against the candidate, checks expanded image capacity and rejects unsupported stock-driver routes. Archives retain their existing read-only held handles, checksums and second-pass identity checks. The later path of a raw file cannot substitute bytes for the captured candidate.

After negotiation and any required DEVINFO authentication, the engine sends:

```text
0x65 / 0                 enter PIT upload
0x65 / 2 [PIT length]    announce exact raw length
raw PIT bytes           exact write, checked 8-byte data acknowledgement
0x65 / 3 [PIT length]    finalize upload, checked acknowledgement
0x65 / 1, /2, /3        retrieve resulting device PIT
```

Every failed, negative, foreign or incomplete response stops the session. There is no automatic retry. The read-back logical PIT must match the candidate even when the caller supplies allocation units. Device routes and capacities are checked again before image commands. Header/entry bytes are compared; transport suffix padding is excluded. Firmware progress totals count firmware bytes and do not count PIT bytes as flashed images.

Serial raw-data replies require type `0x65`. TCP raw PIT data replies accept type `0x65` or generic data type `0`, each with a nonnegative ACK; all PIT control replies still require type `0x65`. [Brokkr's PIT sender](https://github.com/Gabriel2392/brokkr-flash/blob/2118902599857fc22cc559e0cdf73b2357d94fcc/src/protocol/odin/odin_cmd.cpp#L308) uses the generic data-reply path after raw PIT bytes. This implementation rejects foreign, negative and incomplete replies instead of tolerating them.

The stricter raw-data reply check also agrees with an independent implementation: [Heimdall's PIT sender](https://github.com/Benjamin-Dobell/Heimdall/blob/master/heimdall/source/BridgeManager.cpp#L738) expects a PIT response after raw data, and its [response definition](https://github.com/Benjamin-Dobell/Heimdall/blob/master/heimdall/source/ResponsePacket.h#L31) specifies eight bytes and type `0x65`. This corroborates response framing, not physical compatibility or WinUSB support for this port.

The supported protocols are native serial/TCP versions 1–5. Protocols 4/5 require authenticated manufacturer firmware metadata and matching DEVINFO policy before any PIT upload, just as they do before image writes. The reviewed WinUSB image flow remains available for ordinary writes; WinUSB repartition is rejected before protocol commands because its upload boundaries have not been established by the retained evidence.

## Public API and result provenance

Packed request/progress/result sizes remain **64/537/68 bytes**. Check `EJODIN4_GetAbiVersion()==0x1000c` before using the new contract.

- Use `EJODIN4_FLASH_REPARTITION` (operation 3) with `EJODIN4_CLEAN_INSTALL` and complete BL/AP/CP/CSC files. The existing optional USERDATA archive remains supported.
- Optionally add `EJODIN4_PACKAGE_PIT_TARGET` (role 8), whose path names a raw binary PIT of 28 bytes to 1 MiB. It is permitted only in REPARTITION or a complete CLEAN_INSTALL PLAN_ONLY request. It is rejected by ordinary WRITE and READ_PIT.
- Write requests accept only `EJODIN4_FLASH_REBOOT`. For offline embedded-candidate validation, use `EJODIN4_FLASH_PLAN_REPARTITION` (flag 4) with PLAN_ONLY and complete CLEAN_INSTALL firmware. The marker performs no device I/O and is rejected by every device operation. Raw-target plans already imply candidate validation. The previously removed request bit 2 remains rejected. Reserved fields must remain zero.
- `EJODIN4_FLASH_PIT_WRITE_STARTED` (64) means the first PIT command may have reached the device. It also sets `ANY_WRITE`, even if zero firmware bytes were acknowledged.
- `EJODIN4_FLASH_PIT_UPLOADED` (128) proves the final upload acknowledgement. `EJODIN4_FLASH_PIT_VERIFIED` (256) additionally proves matching logical read-back. VERIFIED implies UPLOADED, which implies STARTED and ANY_WRITE.
- Progress phases 18, 19 and 22 report upload intent, upload acknowledgement and verified read-back. The intent callback alone does not prove a write: cancellation at that callback can stop before the first PIT command.

A successful repartition requires all PIT proof bits, all firmware images and bytes acknowledged, and ordinary checked finalization. Cancellation, read-back mismatch or later firmware failure cannot undo a PIT write. The UI and persistent log retain PIT provenance and warn that the partition table may already have changed even when no image completed.

## Verification limits

Tests use synthetic archives and fake transports, including differing old/new PITs, exact command ordering, all upload stages, short/foreign/negative replies, cancellation, raw-target override, read-back mismatch, capacity and route rejection before upload, unsupported transport and missing newer-protocol identity. Real TCP loopback sessions exercise PIT upload and matching/mismatched read-back, bad ACKs and interrupted sessions in listen/connect modes. Owned-window tests exercise offline plans and TCP repartition validation with missing firmware before any confirmation or network operation. No physical device is enumerated.

No physical repartition or recovery was performed. Parsing and host-side capacity checks cannot prove that a PIT is suitable for a particular physical device, storage geometry or bootloader. Unknown addressing, unmeasured zero-capacity USERDATA, unsigned newer-protocol firmware and other existing validation failures remain rejected. See [current verification](VERIFICATION.md) for build and test results.
