# Native Borland verification

The latest application-side logging/lifetime sync is documented in [2026-10-18 synchronization](2026-10-18-sync.md). Native logs are now flushed directly by workers, with timestamps and file-only GUI completion markers. The engine remains linked into the EXE; EasyJTAG's MinGW DLL-unload correction requires no equivalent engine-loader change here. The test runner adds 234 native logger checks per configuration to the eight engine/UI-logic suites described below.

Current firmware-independent PIT behavior is recorded in [PIT routing](PIT_ROUTING.md). The results below verify the rebuilt generic implementation; earlier reviewed-firmware profiles are no longer eligibility gates.

## Build and deployment

Both Win32 configurations compile with classic BCC32 6.90 from Studio 15.0. Debug was built through the actual C++Builder/MSBuild project; Release was built through the Python driver. Each links sixteen Borland OMF objects: UI, eleven engine modules and four unchanged LZ4/xxHash sources. No MSVC/MinGW compiler or private engine DLL is used.

The build checks the pinned vendor source hashes and redistribution notice. Build manifests record source, header, object and executable hashes. Both executables import only ADVAPI32, COMCTL32, COMDLG32, GDI32, KERNEL32, OLE32, SETUPAPI and USER32. WinUSB is loaded from the Windows system directory only when selected. The portable EXE starts successfully when copied alone to a private folder, without any private DLL or Borland RTL package.

## Native tests

All eight suites pass against the rebuilt negotiated-protocol implementation in both Debug and Release, linked directly to the actual engine objects:

| Suite | Checks per configuration |
| --- | ---: |
| Wire/PIT/trailer core | 832 |
| TAR/MD5/LZ4/archive validation | 173 |
| Streaming, hashes, sparse geometry and generic PIT units | 841 |
| Protocol session and integrated flash-operation composition | 11405308 |
| Fake USB and pending-I/O transport | 299 |
| C++03 UI progress/result/severity decisions | 78 |
| Manufacturer signature/parser | 112 |
| DEVINFO identity/policy | 21 |
| Total | 11407664 |

These totals are for the default run. Supplying the optional real package-PIT test input adds four streaming checks, as in the earlier verification snapshot.

Coverage includes exact request goldens, malformed and full-width PIT entries, archive corruption, source checksums, Unicode paths, held-file protection, decoded limits, 8 GiB sparse geometry, ordered repeated writes, protected partitions, second-pass digest checks, callbacks, partial transfers, >4 GiB counters, ACK errors, timeout defaults/overrides, cancellation and checked finalization. Integrated PIT-read tests cover native cleanup failures and throwing callbacks without image writes. SDK structure-size/offset probes also pass with BCC.

The earlier optional S24 package-PIT fixture exercised the now-removed model-specific profile. Current [PIT routing](PIT_ROUTING.md) uses arbitrary input names/digests, matching logical package/live PIT data and per-entry type-2 eMMC 512-byte/type-8 UFS 4096-byte units. Regression coverage checks homogeneous and mixed units, strict caller overrides, expanded/sparse capacity boundaries, mismatched routes, unknown storage types and zero-count USERDATA requiring independent capacity. No fixed model/build/PIT/image hash or inferred 64 GiB USERDATA floor remains in the production path.

The archive fixture reservation was made process-specific after concurrent Debug/Release test runs exposed a temporary-directory collision. Both configurations subsequently passed concurrently.

## UI and real files

The native Debug UI passes seven offline cases; Release additionally validates real firmware and reference-resolution cases. Coverage includes copied EXE without private DLL, retained slot selections, policy/reference rejection, blank selection, nonexistent archive, cancellation, and the actual BL/AP/CP plus HOME_CSC image/reference selection. Its plan contains 26 ordered images and 11882352976 literal bytes; only the metadata reference changes to the clean CSC sibling. The SUPER, HOME CACHE and PRISM SHA256 digests match the earlier independently recorded values. Normal 1140x840 and minimum 1040x760 window captures are readable without overlapping controls.

These tests do not enumerate or open a connected phone. The real plan is offline and has zero acknowledged writes. No new physical Borland flashing test is claimed. The user reported successful flashing and normal Android startup with EasyJTAG; that is separate from this native port verification.

## Repeat

    py tests/run_tests.py --build-app --configuration Debug
    py tests/run_tests.py --build-app --configuration Release

Optional legacy package-PIT fixture and actual firmware (the retained --reviewed-pit option names a test input, not a production allowlist):

    py tests/run_tests.py --configuration Release --reviewed-pit <package.pit> --firmware-dir <firmware-folder>

The supplied engine/rebuild_engine.py delegates to the same classic BCC build. Local detailed results/captures are generated under tests/native-build and tests/ui-artifacts-*; they are excluded from Git. docs/native-build-verification.json and dist/build-manifest.json record the shipped binary identity and source hashes.

## C++Builder Base configuration regression

The IDE can invoke Config=Base. The initial project omitted the inheritance property groups, and its PowerShell wrapper rejected Base before compilation. The project now registers Base/Debug/Release with standard parent metadata, maps Base to Debug for output and build flags, and accepts Base in the wrapper for cached IDE commands.

Actual MSBuild Build invocations pass for Base, Debug and Release. A fresh checkout in a directory containing spaces also passes Config=Base and the exact direct PowerShell -Configuration Base command. No engine source or protocol behavior changed. Repeat with:

    py tests/ide_configuration_tests.py

## 2026-10-17 parity synchronization

See [the source parity table](2026-10-17-sync.md). Updated native session tests independently cover stock request transcripts, exact decoded sequence completion, base-UINT32_MAX totals, versions 1–3 and fail-closed unsupported versions, route-specific sequence caps, and a composed write whose later unsupported route prevents all image writes. Fake transport tests retain WinUSB framing and cover detailed handshake/serial diagnostics. UI logic validates every callback/result and tests the narrow finalized-flash/reboot-warning path and rejection gates. Seven Python manifest-output regression tests cover fallback filenames, stale snapshots and path confinement.

The owned UI additionally checks six retained slot selectors, unchecked duplicates, conflicting references, required policies, cancellation/busy restoration, RichEdit startup without an app-local DLL, concise logs with complete technical session files, and real-firmware offline plans for 26 HOME_CSC images and 23 BL/AP/CP-only images. Reference regressions cover a selected CSC fallback (3 images), a unique CSC metadata neighbor for CP (2 images), missing HOME_CSC mates and corrupt references. No device selection is made and no device operation is requested.

Build/test runners validate the actual manifest-selected executable and source/header/object hashes, including safely published fallback outputs. The shipped portable Release binary and notices are checked against that verified snapshot; source and executable identities are recorded in native-build-verification.json.

## Firmware-independent capacity validation

ABI 1.12 keeps request/progress/result records at 64/537/68 bytes. Mixed-storage WRITE/REPARTITION results use EJODIN4_FLASH_PER_PARTITION_PIT_UNITS (32) with logical_sector_bytes=0; each image is still checked using its own known allocation unit. Homogeneous results expose a common unit. No request flag bypasses capacity validation. These are PIT storage-type conventions, not live READ CAPACITY measurements. Existing checksums and second-pass digests bind transferred bytes to the selected input and are not model eligibility gates. Tests also reject assigning one supplied USERDATA capacity to distinct partition aliases while accepting repeated transfers to the same destination. The build records identify the actual rebuilt executable and test output.

## Negotiated native protocols

The native stock-driver engine supports reviewed protocols 1–5, including capability-selected LZ4 block-record sequences and authenticated DEVINFO model/carrier policy for newer writes. Raw 1–3 and the reviewed WinUSB 1–3 path retain their established transcripts. Current Debug/Release counts are recorded below. The earlier C++Builder Base-to-Debug entry-point verification is historical; it was not repeated for the repartition change.

No genuine manufacturer-signed positive manifest fixture or physical protocol-4/5 run was available. Independent generated-key CryptoAPI vectors test the cryptographic primitive and production rejection of the test key; they do not substitute for that missing positive manufacturer/device evidence. See [native protocol compatibility](NATIVE_PROTOCOL_COMPATIBILITY.md) for source Evidence and exact limits.

## 2026-10-18 explicit repartition verification

The [repartition implementation](REPARTITION.md) follows the retained native host upload sequence and adds stricter raw-data reply checks, candidate preflight, unconditional logical read-back verification and independent PIT-write provenance. Ordinary reference archives remain metadata-only. An explicit offline-only planning marker checks embedded candidates without changing ordinary plan semantics.

Both commands passed on the final source/header snapshot:

```powershell
py tests/run_tests.py --build-app --configuration Debug
py tests/run_tests.py --build-app --configuration Release
```

Each configuration compiled and linked sixteen BCC32 objects and passed all eight native suites: core 832; archive 173; streaming 979; protocol/session 12,974,522; transport 299; signed identity 112; DEVINFO 21; UI decisions 152. Total: **12,977,090 native checks per configuration**, plus 234 persistent-log checks, seven manifest regressions and thirteen owned-window offline cases at 1140x840 and 1040x760. Python syntax checks and `git diff --check` passed. Existing compiler warnings in the unchanged guard/vendor patterns remain; no compile errors. No new dependencies or source cleanup were introduced.

The new fault matrix covers exact upload controls/raw bytes, every upload acknowledgement, short writes/replies, foreign/negative responses, cancellation before and after PIT side effects, failed or mismatched read-back even with caller units, raw override and malformed embedded PITs, unsupported stock routes/capacity before upload, missing newer identity and WinUSB rejection. Offline UI regressions additionally reject oversized or unsupported embedded candidates before connection. Independent architecture and final code reviews passed after the embedded-plan correction; final visual verdict is 93/100.

The portable `dist/OdinBCC.exe` and matching manifest are published from the verified Release snapshot. [Build evidence](native-build-verification.json) records binary/source/header/object hashes, imports, per-suite outputs and remaining limits. No phone was enumerated, probed, read, repartitioned or flashed. Standalone PIT-only writes, partial-firmware repartition, WinUSB repartition and XML conversion remain unsupported. Optional real-firmware offline suites and IDE entry-point tests were not rerun for this feature; synthetic tests do not establish physical compatibility or recovery.
