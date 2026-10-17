# Odinus Borlandus

Standalone Samsung Download-mode firmware application, built entirely with **classic Borland/Embarcadero BCC32 6.90** from C++Builder XE7 / Studio 15.0.

The UI, archive validation, LZ4 decoding, PIT/layout validation, session protocol, Samsung stock-driver transport and WinUSB transport compile into **one native x86 executable**. There is no private Odin engine DLL, DLL loader, MinGW engine or MSVC build dependency. Standard Windows system libraries are used normally.

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

1. Choose firmware archives. Empty image slots remain unselected under Update selected slots. CSC / HOME_CSC is detected from its filename; USERDATA is an archive slot, not an instruction to write userdata.img.
2. Prepare an offline plan before writing. A PIT reference supplies metadata only. A PIT-less HOME_CSC reference resolves to its exact matching CSC sibling without adding CSC image transfers.
3. For device operations, connect Download mode, Refresh and explicitly select a supported Samsung modem or WinUSB interface.
4. Check link only performs the handshake; Read device PIT performs a read session. Flash uses the selected data policy and asks for confirmation before writing.
5. Progress separates verification, overall acknowledged bytes and current-image acknowledged bytes. Milestones/errors go to the log; packet samples do not flood it. Session logs are saved under the executable's logs/ folder.
6. Cancel requests safe engine cancellation. It cannot undo writes; an unhealthy driver may take time to release pending I/O.

Update selected slots excludes USERDATA, MISC and OMR destinations. Preserve mode requires the OEM download list. Clean installation permits data erasure. No package PIT is uploaded and no repartition operation is exposed. Source hashes, route consistency, live/package PIT matching and capacity checks remain enforced. Automatic geometry is restricted to the reviewed SM-S921B BYH2 layout; other layouts require verified geometry and are rejected by this simple UI when unresolved.

The source port preserves 1 MiB modern Odin packets, 30 MiB sequences and the negotiated 120-second data/ACK timeout. No 32/64 KiB write-size workaround or Knox-policy bypass is introduced. Successful offline tests do not establish that the earlier physical PRISM/Knox issue is fixed.

See tests/ and docs/VERIFICATION.md for repeatable validation and its limits.

## Tests

    py tests/run_tests.py --build-app --configuration Debug
    py tests/run_tests.py --build-app --configuration Release

Optional --reviewed-pit <package.pit> and --firmware-dir <folder> enable real-file offline coverage. The ready-to-run portable build is in [dist/](dist/). No operation starts automatically.
