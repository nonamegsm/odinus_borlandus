# Native Borland verification

## Build and deployment

Both Win32 configurations compile with classic BCC32 6.90 from Studio 15.0. Debug was built through the actual C++Builder/MSBuild project; Release was built through the Python driver. Each links fifteen Borland OMF objects: UI, ten engine modules and four unchanged LZ4/xxHash sources. No MSVC/MinGW compiler or private engine DLL is used.

The build checks the pinned vendor source hashes and redistribution notice. Build manifests record source, header, object and executable hashes. Both executables import only ADVAPI32, COMCTL32, COMDLG32, GDI32, KERNEL32, OLE32, SETUPAPI and USER32. WinUSB is loaded from the Windows system directory only when selected. The portable EXE starts successfully when copied alone to a private folder, without any private DLL or Borland RTL package.

## Native tests

All six suites pass in both Debug and Release, linked directly to the actual engine objects:

| Suite | Checks per configuration |
| --- | ---: |
| Wire/PIT/trailer core | 832 |
| TAR/MD5/LZ4/archive validation | 173 |
| Streaming, hashes, sparse geometry and reviewed layout | 125 |
| Protocol session and integrated flash-operation composition | 1392767 |
| Fake USB and pending-I/O transport | 299 |
| C++03 UI progress/result/severity decisions | 56 |
| Total | 1394252 |

Coverage includes exact request goldens, malformed and full-width PIT entries, archive corruption, source checksums, Unicode paths, held-file protection, decoded limits, 8 GiB sparse geometry, ordered repeated writes, protected partitions, second-pass digest checks, callbacks, partial transfers, >4 GiB counters, ACK errors, timeout defaults/overrides, cancellation and checked finalization. Integrated PIT-read tests cover native cleanup failures and throwing callbacks without image writes. SDK structure-size/offset probes also pass with BCC.

The optional actual reviewed S24 package PIT was supplied to the layout tests. It checks pinned package/live layout matching, caller units, inferred 4096-byte units, selected/full-mode USERDATA rules, tampered geometry, fingerprints, build descriptors, extents and vendor_boot hashes.

The archive fixture reservation was made process-specific after concurrent Debug/Release test runs exposed a temporary-directory collision. Both configurations subsequently passed concurrently.

## UI and real files

The native Debug UI passes seven offline cases; Release additionally validates real firmware and reference-resolution cases. Coverage includes copied EXE without private DLL, retained slot selections, policy/reference rejection, blank selection, nonexistent archive, cancellation, and the actual BL/AP/CP plus HOME_CSC image/reference selection. Its plan contains 26 ordered images and 11882352976 literal bytes; only the metadata reference changes to the clean CSC sibling. The SUPER, HOME CACHE and PRISM SHA256 digests match the earlier independently recorded values. Normal 1140x840 and minimum 1040x760 window captures are readable without overlapping controls.

These tests do not enumerate or open a connected phone. The real plan is offline and has zero acknowledged writes. No new physical Borland flashing test is claimed. The user reported successful flashing and normal Android startup with EasyJTAG; that is separate from this native port verification.

## Repeat

    py tests/run_tests.py --build-app --configuration Debug
    py tests/run_tests.py --build-app --configuration Release

Optional reviewed package PIT and actual firmware:

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
