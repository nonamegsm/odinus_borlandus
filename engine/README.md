# Native Borland engine

engine/src is the actual classic BCC32 port, compiled directly into OdinBCC.exe. The public C-compatible records retain their checked sizes, but they are ordinary in-process calls: no engine DLL is built, loaded or distributed.

Unsupported C++11/20 features were replaced with explicit constructors, typed iterators, callback functions/context objects and fixed arrays. File handles, overlapped I/O lifetimes, checksums, sparse geometry, PIT/profile validation, transfer framing, sequence ACKs, cancellation and truthful partial-write state are retained.

Build from the project root:

    py -3 build.py --configuration Debug
    py -3 build.py --configuration Release

C++Builder Studio 15.0 / BCC32 6.90 compiles every engine translation unit and all four LZ4/xxHash C sources; no other C++ compiler is required. LZ4 sources are unchanged and compile in BCC C++ mode. The unmodified modern upstream snapshot is reference/provenance material only. Root LICENSE/NOTICE and vendor BSD notices must accompany redistribution.

BccCompat.h corrects the classic stdint UINT64_C macro so literals above 4 GiB retain their real width. Non-owning callbacks are synchronous; their context lifetime covers the whole call. FirmwareSet explicitly owns/deletes retained archives; image readers are destroyed before those archives.

This is a source/compiler port, not proof of successful physical flashing or a Knox-policy fix. Test commands and evidence are in ../tests and ../docs/VERIFICATION.md.
