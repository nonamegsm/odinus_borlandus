# Native offline verification

Run with Python3 from the project folder:

    py tests/run_tests.py --build-app --configuration Debug

This builds the native classic Borland engine and runs tests/native_tests.py, followed by owned-window UI checks. No engine DLL is loaded. Use --native-only to skip UI automation, --configuration Release for Release objects, and --bds to override the installed Studio15.0 location.

The static core probe retains832 independent wire/PIT/trailer golden and error checks. Native archive tests cover173 strict TAR/MD5/LZ4/PIT, cancellation, Unicode, file-sharing and progress checks. Session, transport, firmware-streaming and C++03 UI logic targets are included by native_tests.py. Manifest-output tests additionally reject stale binaries and changed source/header/object snapshots. Every test uses local files or fake transports; no connected phone is accessed.

Optional actual firmware offline plan:

    py tests/run_tests.py --configuration Debug --firmware-dir F:/Firmware_Test/SM-S921B_PLS_S921BXXS9BYH2

The owned UI tests copy the EXE alone to a private directory and require native startup without a private DLL. They test blank selections, missing files, offline cancellation, normal1140x840 and minimum1040x760 layouts, and optional26-image/11,882,352,976-byte BL/AP/CP/HOME_CSC plan. Only Prepare offline plan and Cancel may be activated. Captures briefly show the owned window without activation and reject blank images. No device enumeration, connection probe, device PIT read or firmware write is requested.

Results and screenshots are under tests/native-build/{configuration} and tests/ui-artifacts-{configuration}. PNG is optional if Pillow is already installed; no dependency installation is needed.

compiler-feature-results.json and core-source-compile-probe.log are historical evidence explaining why a source port was needed. The current engine contains classic-compatible implementations and does not depend on the former modern engine DLL.

The UI runner selects the executable recorded in the verified build manifest rather than assuming OdinBCC.exe; this covers builds that safely publish OdinBCC.native.exe while an older copy is running. Native UI logic tests cover slot selection, progress/result validation and the narrowly accepted post-finalization reboot warning without loading a real device driver.
