# Firmware-independent PIT routing

For explicit PIT upload and read-back verification in ABI 1.12, see [repartition usage](REPARTITION.md). The [historical REA audit](REPARTITION_AUDIT.md) explains the native reference behavior. Metadata-only PIT references remain distinct from upload targets.

Firmware eligibility does not depend on a known model, firmware release name, package-PIT fingerprint or VENDOR_BOOT digest. Image hashes still protect the actual selected archive between validation and transfer; they are computed from that input and are not compared with a firmware allowlist.

## Allocation units and capacity

The automatic path first requires matching logical package/live PIT bytes, excluding transport suffix padding. Each selected type-2 MMC route uses a 512-byte PIT allocation unit; each selected type-8 UFS route uses 4096 bytes. Positive caller-provided units (512, 1024, 2048 or 4096 bytes) override automatic selection for supported storage types and retain strict exact-route and capacity checks. A failing override does not silently fall back to automatic units. The engine compares expanded image size, including Android sparse expansion, with the live partition block count multiplied by the applicable unit.

This is the Samsung PIT storage-type convention. Independent multi-device evidence is recorded in [postmarketOS pmaports merge request 5365](https://gitlab.com/postmarketOS/pmaports/-/merge_requests/5365), where the maintainer derives partition sizes from PIT storage type and block count across several devices. This evidence supports the implemented convention; it is not a live READ CAPACITY measurement or proof for every possible Samsung addressing scheme. The overloaded PIT block-size/offset field is not treated as an independent sector-size measurement; see [Heimdall's PIT definition](https://github.com/Benjamin-Dobell/Heimdall/blob/master/libpit/source/libpit.h).

The C contract is ABI 1.12 (65548), with unchanged 64/537/68-byte request/progress/result records. Mixed MMC/UFS plans use units per selected partition. Their result has logical_sector_bytes=0 and, for WRITE/REPARTITION, EJODIN4_FLASH_PER_PARTITION_PIT_UNITS (32), rather than reporting a false common unit. Homogeneous plans report their common unit. Unknown storage types remain unsupported by this capacity validator. A zero block count does not establish capacity; USERDATA requires independently supplied capacity rather than an invented minimum. That capacity must align to the selected unit and cannot contradict a positive live PIT capacity; other zero-count destinations remain rejected. Selected-slot policy still excludes USERDATA, MISC and OMR.

## Native Odin observations

The current native analysis binds Odin3_v3.14.4.exe SHA256 aa17ccf37d52c816bcea6e54dbdd6daf90fe2ac984641d01034ade77ff9dcb41 through owned REA/headless IDA sessions. Mapping and raw/encoded transfers use image filename and PIT routing fields rather than a fixed device/build/image-digest eligibility list. The evidence records, functions, protocol matrix and limitations are recorded in [native protocol compatibility](NATIVE_PROTOCOL_COMPATIBILITY.md). Static host analysis does not establish a live capacity measurement or universal bootloader behavior.

## Native engine checks and limits

[Layout resolution](../engine/src/Odin4LayoutProfile.cpp) uses current PIT identity and storage type. [Partition validation](../engine/src/Odin4FirmwareStream.cpp) retains expanded-size, exact-route, selected-policy and capacity checks. [The session](../engine/src/Odin4Session.cpp) separately validates supported transport routes before the first image write and requires packet acknowledgements and finalization.

[Streaming regressions](../tests/native_streaming_tests.cpp) cover arbitrary archive names and digests, MMC/UFS and mixed units, logical-PIT mismatches, strict caller overrides, capacity boundaries, sparse extents and unknown addressing. [Session regressions](../tests/native_session_tests.cpp) exercise the composed operation with fake transports. Synthetic tests verify implementation behavior; they are not independent evidence for the allocation-unit convention and do not constitute physical flashing validation.

Firmware independence does not mean all protocol dialects are implemented. Native stock-driver protocols 1–5 use the negotiated raw/encoded flow, with authenticated DEVINFO policy for newer writes; WinUSB remains at reviewed protocols 1–3. Automatic capacity handling currently covers MMC/UFS routes only, and zero-count USERDATA remains unresolved without verified capacity. See [verification](VERIFICATION.md) for the remaining transport and dialect limits.

## Verification on 2026-10-17

Current builds link sixteen classic BCC objects and pass eight native targets in both Debug and Release, including the optional real PIT fixture: core 832; archive 173; streaming 845; protocol/session 11,405,308; transport 299; signed identity 112; DEVINFO 21; UI decisions 78, totaling 11,407,668 checks per configuration. Seven Debug and thirteen Release offline UI cases also pass. The IDE Base selection builds Debug through the real project entry point.

Full build identities, imports and limits are recorded in native-build-verification.json and [verification](VERIFICATION.md). No connected phone was enumerated, read or flashed.
