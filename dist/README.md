# Portable native Borland build

Run OdinBCC.exe on Windows. It contains the complete BCC32 engine and needs no private Odin DLL or compiler RTL package. Keep LICENSE and NOTICE with the executable. Standard Windows libraries and the selected phone driver are still required.

The native engine and hardware-free tests pass; physical flashing and earlier PRISM/Knox behavior remain unverified. No phone operation starts automatically.

Rebuild from the source project with build.ps1 or OdinBCC.cbproj. build-manifest.json records the compiler, source/header hashes, object hashes and Windows imports for this binary.
