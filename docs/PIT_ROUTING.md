# Firmware-independent PIT routing

Firmware eligibility does not depend on a known model, firmware release name, package-PIT fingerprint or VENDOR_BOOT digest. Image hashes still protect the actual selected archive between validation and transfer; they are computed from that input and are not compared with a firmware allowlist.

## Allocation units and capacity

The automatic path first requires matching logical package/live PIT bytes, excluding transport suffix padding. Each selected type-2 MMC route uses a 512-byte PIT allocation unit; each selected type-8 UFS route uses 4096 bytes. Positive caller-provided units (512, 1024, 2048 or 4096 bytes) override automatic selection for supported storage types and retain strict exact-route and capacity checks. A failing override does not silently fall back to automatic units. The engine compares expanded image size, including Android sparse expansion, with the live partition block count multiplied by the applicable unit.

This is the Samsung PIT storage-type convention. Independent multi-device evidence is recorded in [postmarketOS pmaports merge request 5365](https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5365), where the maintainer derives partition sizes from PIT storage type and block count across several devices. This evidence supports the implemented convention; it is not a live READ CAPACITY measurement or proof for every possible Samsung addressing scheme. The overloaded PIT block-size/offset field is not treated as an independent sector-size measurement; see [Heimdall's PIT definition](https://github.com/Benjamin-Dobell/Heimdall/blob/master/libpit/source/libpit.h).

The shared C contract is ABI 1.10 (65546), with unchanged 64/537/68-byte request/progress/result records. Mixed MMC/UFS plans use units per selected partition. Their result has logical_sector_bytes=0 and, for WRITE, EJODIN4_FLASH_PER_PARTITION_PIT_UNITS (32), rather than reporting a false common unit. Homogeneous plans report their common unit. Unknown storage types remain unsupported by this capacity validator. A zero block count does not establish capacity; USERDATA requires independently supplied capacity rather than an invented minimum. That capacity must align to the selected unit and cannot contradict a positive live PIT capacity; other zero-count destinations remain rejected. Selected-slot policy still excludes USERDATA, MISC and OMR.

## Native Odin observations

The REA workflow was applied to the supplied Odin download DLL, without executing that DLL or accessing a phone. REA admitted the binary, but its native adapter expected a different IDA supervisor API from the installed one. Static decompilation therefore used the installed IDA supervisor directly. These are IDA observations under the REA workflow, not successful REA function-dossier Evidence records. Complete tool outputs are retained under .omx/research/universal-routing/.

The inspected chain is Odin_StartDownload -> the COdin3HomeImp download entry -> worker 0x10022910 -> state machine 0x10018DB0. Within that chain:

| Function | Observed behavior |
| --- | --- |
| 0x10020500 | Requests the device PIT with 0x65/1, reads blocks with 0x65/2 and completes with 0x65/3. |
| 0x1001BDE0 | Walks 132-byte PIT entries after the 28-byte header and feeds them into image mapping. |
| 0x1001BB90 | Matches an image filename against the PIT filename and copies the selected routing fields. |
| 0x10020FC0 | Selects sequence limits by PIT device type: 30 MiB for 1/2/8, 4 MiB for 6, 1 MiB for 7. Other branches have distinct behavior. |
| 0x1001C560 | Sends raw image sequences using 0x66/0, 0x66/2 and 0x66/3; completion carries decoded sequence bytes, binary type, device type, identifier and the last-sequence flag. |

No model/build/image-digest allowlist was observed in these mapping and raw-transfer functions. This is a bounded static finding, not an exhaustive claim about every path in the application. In particular, these functions do not independently establish the MMC/UFS byte-unit convention above, and static host analysis does not prove how every device bootloader enforces capacity.

## Native engine checks and limits

[Layout resolution](../engine/src/Odin4LayoutProfile.cpp) uses current PIT identity and storage type. [Partition validation](../engine/src/Odin4FirmwareStream.cpp) retains expanded-size, exact-route, selected-policy and capacity checks. [The session](../engine/src/Odin4Session.cpp) separately validates supported transport routes before the first image write and requires packet acknowledgements and finalization.

[Streaming regressions](../tests/native_streaming_tests.cpp) cover arbitrary archive names and digests, MMC/UFS and mixed units, logical-PIT mismatches, strict caller overrides, capacity boundaries, sparse extents and unknown addressing. [Session regressions](../tests/native_session_tests.cpp) exercise the composed operation with fake transports. Synthetic tests verify implementation behavior; they are not independent evidence for the allocation-unit convention and do not constitute physical flashing validation.

Firmware independence does not mean all protocol dialects are implemented. Raw protocols 1–3 remain supported; versions 4/5 still require DEVINFO handling. Automatic capacity handling currently covers MMC/UFS routes only, and zero-count USERDATA remains unresolved without verified capacity. See [verification](VERIFICATION.md) for the remaining transport and dialect limits.

## Verification on 2026-10-17

Classic BCC32 6.90 rebuilt all 15 native objects and linked Debug and Release application outputs. Both configurations passed seven build-output tests and all six native targets: core (832 checks), archives (173), streaming/layout (164), session fake transport (1,667,221), transport (299), and UI logic (66), totaling 1,668,755 checks per configuration. Debug used the actual C++Builder Base-to-Debug project path; Release used the Python driver. Build manifests verified executable/source/header/object consistency. Owned UI tests passed seven Debug and thirteen Release offline cases, including real-file firmware plans.

Detailed final logs are retained in the EasyJTAG workspace under .omx/research/odin-generic-pit-routing/bcc-final-{debug,release}.log. The normal application is bin/Release/OdinBCC.exe and the verified portable copy is dist/OdinBCC.exe. The build retains existing warnings in unchanged LZ4/xxHash vendor sources. No connected device was enumerated, probed, read or flashed; no physical-device success is claimed.
