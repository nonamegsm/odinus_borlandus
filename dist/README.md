# Portable native Borland build

Run OdinBCC.exe on Windows. The GUI, Odin raw protocol engine and pinned LZ4 sources are compiled directly with classic BCC32 6.90 into one native x86 executable. No private Odin DLL or compiler RTL package is required. Keep LICENSE and NOTICE with the executable. Standard Windows libraries, including the system RichEdit control, and the selected phone driver are still required.

This build synchronizes the 2026-10-17 stock-driver framing, raw protocol 1–3 guards, slot selection, supported-device filtering, concise coloured logging and finalized-flash/reboot-warning fixes from EasyJTAG. Versions 4/5 remain unsupported until their DEVINFO flow is implemented. No phone operation starts automatically.

Native Debug/Release tests and owned-window offline firmware-plan checks pass. This native Borland build has not been physically flashed; the user-reported successful EasyJTAG flash is separate evidence.

Rebuild from the source project with build.ps1 or OdinBCC.cbproj. build-manifest.json records the shipped executable, compiler, source/header/object hashes, notices and Windows imports.
