# Portable native Borland build

Run OdinBCC.exe on Windows. The GUI, Odin raw protocol engine and pinned LZ4 sources are compiled directly with classic BCC32 6.90 into one native x86 executable. No private Odin DLL or compiler RTL package is required. Keep LICENSE and NOTICE with the executable. Standard Windows libraries, including the system RichEdit control, and the selected phone driver are still required.

This build synchronizes the 2026-10-17 stock-driver framing, raw protocol 1–3 guards, slot selection, supported-device filtering, concise coloured logging and finalized-flash/reboot-warning fixes from EasyJTAG. Native stock protocols 1–5 now select the appropriate raw/encoded flow; protocols 4/5 require manufacturer-authenticated package metadata and DEVINFO policy. WinUSB remains at reviewed protocols 1–3. No phone operation starts automatically.

PIT capacity validation is firmware-independent: eMMC type 2 uses 512-byte units and UFS type 8 uses 4096-byte units per partition. Expanded images must fit the matched live layout; zero-count USERDATA requires independently supplied capacity. ABI 1.11 supports mixed units without a model/build/hash allowlist.

Native Debug/Release tests and owned-window offline firmware-plan checks pass. This native Borland build has not been physically flashed; the user-reported successful EasyJTAG flash is separate evidence.

Rebuild from the source project with build.ps1 or OdinBCC.cbproj. build-manifest.json records the shipped executable, compiler, source/header/object hashes, notices and Windows imports.

The 2026-10-18 application sync adds timestamped UTF-8 session logs, flushed directly by workers before GUI messages. Completion/join markers are file-only, and GUI processing does not duplicate saved messages. The engine remains statically linked; EasyJTAG's MinGW DLL-unload correction needs no equivalent DLL pin in this executable. Close the app before replacing the portable EXE.
