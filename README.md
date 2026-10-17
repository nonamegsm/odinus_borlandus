# Odinus Borlandus

Standalone Samsung Download-mode firmware application, built entirely with **classic Borland/Embarcadero BCC32 6.90** from C++Builder XE7 / Studio 15.0.

The UI, archive validation, LZ4 decoding, PIT/layout validation, session protocol, Samsung stock-driver transport and WinUSB transport compile into **one native x86 executable**. There is no private Odin engine DLL, external engine loader, MinGW engine or MSVC build dependency. Standard Windows system libraries are used normally; the coloured log uses the Windows RichEdit control.

## Build

Requirements: Windows, Python 3 and C++Builder Studio 15.0. No packages or downloads are required.

~~~powershell
.\build.ps1 -Configuration Debug
.\build.ps1 -Configuration Release
.\bin\Release\OdinBCC.exe
~~~

Or use Python:

~~~powershell
py -3 build.py --configuration Release --bds 'C:\Program Files (x86)\Embarcadero\Studio\15.0'
~~~

Open **OdinBCC.cbproj** in C++Builder, choose Win32 and Debug or Release, then Build/Run. IDE and CLI builds share the same response-file compiler driver. The IDE Base inheritance node is also accepted and builds with Debug settings and output paths. Both engine modules and the four pinned LZ4/xxHash C sources compile with BCC; the latter use its C++ mode without changing their source.

~~~powershell
& "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" .\OdinBCC.cbproj /t:Build /p:Config=Release /p:Platform=Win32
~~~

Compiler-root precedence: --bds / -Bds, BDS environment variable, then the Studio 15.0 default above. The PowerShell wrapper accepts -Python with a full Python executable path.

## Project layout

- src/Main.cpp: simple Win32 UI; native engine calls, worker thread, progress and cancellation.
- engine/src/: actual downgraded Borland engine implementation.
- engine/src/BccCompat.h: bounded fixed arrays, synchronous callbacks and correct 64-bit literal macros for the old RTL.
- engine/include/ and include/: native packed request/progress records.
- engine/vendor/lz4/: pinned LZ4/xxHash source and BSD notices, compiled into the application.
- engine/vendor/upstream/: unmodified upstream reference snapshot, retained for provenance; not compiled.
- tests/: native core/archive/stream/session tests and hardware-free UI tests.
- LICENSE and NOTICE: redistribution terms and third-party attribution.

Outputs live under bin/Debug and bin/Release; objects/response files live under build/. The build manifest records executable and source hashes. Distribute the executable with LICENSE and NOTICE. No private engine DLL or compiler RTL package needs to be installed.

## Use

1. Enable the archive slots you want to use and choose their firmware files. Unchecking a slot retains its path and disables its controls. Unchecked/empty image slots remain unselected under Update selected slots; an unchecked CSC can supply PIT metadata without image writes. CSC / HOME_CSC is detected from its filename; USERDATA is an archive slot, not an instruction to write userdata.img.
2. Prepare an offline plan before writing. A PIT reference supplies metadata only. A PIT-less HOME_CSC reference resolves to its exact matching CSC sibling without adding CSC image transfers.
3. For device operations, connect Download mode, Refresh and explicitly select a supported Samsung modem or WinUSB interface.
4. Check link only performs the handshake; Read device PIT performs a read session. Flash uses the selected data policy and asks for confirmation before writing.
5. Progress separates verification, overall acknowledged bytes and current-image acknowledged bytes. The coloured log shows one line per ordered image and concise milestones/results, without emoticons. Full technical session logs are saved under the executable's logs/ folder.
6. Cancel requests safe engine cancellation. It cannot undo writes; an unhealthy driver may take time to release pending I/O.

Update selected slots excludes USERDATA, MISC and OMR destinations. Preserve mode requires the OEM download list. Clean installation permits data erasure. No package PIT is uploaded and no repartition operation is exposed. Source hashes, route consistency, live/package PIT matching and capacity checks remain enforced. Firmware eligibility does not depend on model names, firmware release names or known-image hashes. Automatic PIT allocation units follow storage type: 512 bytes for type-2 MMC and 4096 bytes for type-8 UFS, independently for each selected partition. Unknown addressing and zero-capacity destinations without independently supplied capacity remain rejected. See [PIT routing evidence and limits](docs/PIT_ROUTING.md).

The native stock-driver path uses the reviewed negotiation/framing sequence, exact decoded completion lengths and route-dependent sequence limits (30 MiB for types 1/2/8, 4 MiB for type 6 and 1 MiB for type 7). Modern packets remain 1 MiB with the 120-second data/ACK timeout. The native stock-driver flow supports negotiated protocols 1–5, with raw fallback or capability-selected LZ4 block records. Protocol 4/5 writes require authenticated manufacturer metadata and matching DEVINFO model/carrier policy. The existing WinUSB 1–3 flow remains supported. See [native protocol compatibility](docs/NATIVE_PROTOCOL_COMPATIBILITY.md). Every planned route is checked before the first image write. No 32/64 KiB workaround or Knox-policy bypass is introduced.

A fully acknowledged and finalized stock-driver flash whose separate reboot confirmation fails is reported as completed with a warning; the reboot flag remains unconfirmed. Incomplete writes, failed finalization, unexpected replies, invalid metadata and cancellation remain errors. The user reported successful flashing and normal Android startup with the EasyJTAG implementation; this native Borland synchronization has not been physically flashed.

See tests/ and docs/VERIFICATION.md for repeatable validation and its limits.

## Tests

    py tests/run_tests.py --build-app --configuration Debug
    py tests/run_tests.py --build-app --configuration Release

Optional --reviewed-pit <package.pit> and --firmware-dir <folder> enable real-file offline coverage. The ready-to-run portable build is in [dist/](dist/). No operation starts automatically.

The test runner verifies the manifest-named executable and its source/header/object snapshot before testing, including the fallback filename used when an older executable is locked. See [today's parity record](docs/2026-10-17-sync.md) for the source commits covered.
