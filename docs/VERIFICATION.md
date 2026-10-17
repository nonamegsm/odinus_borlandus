# Native Borland verification

## Build and deployment

Both Win32 configurations compile with classic BCC32 6.90 from Studio 15.0. Debug was built through the actual C++Builder/MSBuild project; Release was built through the Python driver. Each links fifteen Borland OMF objects: UI, ten engine modules and four unchanged LZ4/xxHash sources. No MSVC/MinGW compiler or private engine DLL is used.

The build checks the pinned vendor source hashes and redistribution notice. Build manifests record source, header, object and executable hashes. Both executables import only ADVAPI32, COMCTL32, COMDLG32, GDI32, KERNEL32, OLE32, SETUPAPI and USER32. WinUSB is loaded from the Windows system directory only when selected. The portable EXE starts successfully when copied alone to a private folder, without any private DLL or Borland RTL package.

## Native tests

All five suites pass in both Debug and Release, linked directly to the actual engine objects:

| Suite | Checks per configuration |
| --- | ---: |
| Wire/PIT/trailer core | 832 |
| TAR/MD5/LZ4/archive validation | 173 |
| Streaming, hashes, sparse geometry and reviewed layout | 125 |
| Protocol session and integrated flash-operation composition | 575404 |
| Fake USB and pending-I/O transport | 287 |
| Total | 576821 |

Coverage includes exact request goldens, malformed and full-width PIT entries, archive corruption, source checksums, Unicode paths, held-file protection, decoded limits, 8 GiB sparse geometry, ordered repeated writes, protected partitions, second-pass digest checks, callbacks, partial transfers, >4 GiB counters, ACK errors, timeout defaults/overrides, cancellation and checked finalization. Integrated PIT-read tests cover native cleanup failures and throwing callbacks without image writes. SDK structure-size/offset probes also pass with BCC.

The optional actual reviewed S24 package PIT was supplied to the layout tests. It checks pinned package/live layout matching, caller units, inferred 4096-byte units, selected/full-mode USERDATA rules, tampered geometry, fingerprints, build descriptors, extents and vendor_boot hashes.

The archive fixture reservation was made process-specific after concurrent Debug/Release test runs exposed a temporary-directory collision. Both configurations subsequently passed concurrently.

## UI and real files

The native Release UI passes five offline cases: copied EXE without private DLL, blank selection, nonexistent archive, cancellation, and the actual BL/AP/CP plus HOME_CSC image/reference selection. Its plan contains 26 ordered images and 11882352976 literal bytes; only the metadata reference changes to the clean CSC sibling. The SUPER, HOME CACHE and PRISM SHA256 digests match the earlier independently recorded values. Normal 1140x840 and minimum 1040x760 window captures are readable without overlapping controls.

These tests do not enumerate or open a connected phone. The real plan is offline and has zero acknowledged writes. No physical flashing or fix for the earlier PRISM/Knox device error is claimed.

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
