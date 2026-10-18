# REPARTITION and reference PIT audit

Date: 2026-10-18. Source baseline: `f9aeb51` on `main`.

## Result

**OdinBCC has no REPARTITION implementation. Its PIT reference is routing metadata, not a PIT upload request.** Native Odin 3.14.4 has a separate PIT upload stage before it retrieves the device PIT and maps images. That stage is absent from this port. Selecting a reference archive, choosing Clean installation, or supplying a sector-size override cannot enable it.

This is an intentional restriction in the current public contract, but a concrete feature gap against native Odin. It is not evidence of a broken, already implemented repartition branch. No reported device error or failed-session capture was supplied, so this investigation does not attribute a particular physical failure to that gap.

This commit adds the report and retained evidence only. Production behavior and ABI remain unchanged.

## Evidence and scope

REA 6.4.0 inspected `Odin3_v3.14.4/Odin3_v3.14.4.exe` through the existing task-local headless IDA supervisor configuration. SHA-256 before and after analysis:

```text
aa17ccf37d52c816bcea6e54dbdd6daf90fe2ac984641d01034ade77ff9dcb41
```

The executable was not run; no phone was enumerated, opened, read, erased or flashed. The owned analysis session closed successfully. The initially connected REA server had no configured native provider; a separate REA process reused the already installed compatible IDA setup without changing global configuration or installing dependencies. An initial provider health response was incomplete during startup; subsequent live queries succeeded after readiness.

[Evidence index and fresh test results](evidence/repartition/index.json) map each label below to its full REA Evidence ID. [The evidence bundle](evidence/repartition/rea-evidence.json) preserves 22 complete, unchanged records selected from the exported session, including pseudocode, raw provider observations and limitations. Other exploratory records are omitted. Function addresses are virtual addresses for this exact artifact, not source symbols. Pseudocode is recovered analysis, not original Samsung source.

## Findings in the port

| Priority | Finding | Evidence and consequence |
| --- | --- | --- |
| High: missing parity | No repartition operation or flag | [Public ABI](../include/EasyJtagOdin4Core.h#L50) exposes PLAN_ONLY, READ_PIT and WRITE, and only the REBOOT request flag. [Request validation](../engine/src/Odin4FlashOperation.cpp#L30) rejects unknown operations, flag bit 2 and reserved fields before device I/O. |
| High: missing parity | No PIT upload stage | [Session startup](../engine/src/Odin4Session.cpp#L104) implements PIT download with `0x65/1`, indexed `0x65/2`, then `0x65/3`. [Flash execution](../engine/src/Odin4FlashOperation.cpp#L223) validates that live layout and proceeds to image sending. There is no corresponding native `0x65/0` upload flow. The unused vendored `RQT_PIT_SET=0` enum does not implement one. |
| Expected guard, not a defect | A different target layout is rejected | [Automatic layout resolution](../engine/src/Odin4LayoutProfile.cpp#L16) requires identical logical package/live PIT bytes, excluding suffix padding. A changed header or entry fails with `Automatic PIT units require valid selected packages and matching logical package/live PIT`. This protects an existing-layout flash; it cannot perform layout migration. |
| Medium: input mismatch | A reference PIT is an archive input | [The GUI picker](../src/Main.cpp#L265) uses the firmware TAR/TAR.MD5 filter; [role 7](../engine/src/Odin4FirmwareArchive.cpp#L774) goes through archive inspection. A bare `.pit` selected here is not the equivalent of Odin's manual PIT file picker. |
| Medium: expectation mismatch | Reference selection never overrides or writes a PIT | [Firmware preparation](../engine/src/Odin4FirmwareStream.cpp#L314) skips reference images; [PIT processing](../engine/src/Odin4FirmwareStream.cpp#L335) retains the PIT for routing and records `SKIP_PIT`. Different embedded/reference PIT byte arrays fail with `Conflicting package PIT members`; the explicit reference does not take precedence. [The GUI](../src/Main.cpp#L249) explicitly reports `PIT reference: loaded (not flashed)`. |

An explicit `logical_sector_bytes` value bypasses the automatic whole-table equality check, but [selected route validation](../engine/src/Odin4FirmwareStream.cpp#L248) still requires the actual selected live route to match the package route, including geometry. That override supplies allocation units; it never writes a partition table. It is not an escape hatch for repartitioning.

The separate read-only archive analysis API can accept an external raw PIT ([declaration](../include/EasyJtagOdin4Core.h#L365), [implementation](../engine/src/Odin4FirmwareArchive.cpp#L670)). That API must not be confused with the flash request, which has no raw external-PIT field. Clean-install data policy also does not imply partition-table replacement.

## Native Odin: option and PIT source selection

The manual checkbox and automatic package-PIT path are distinct. Treating the INI checkbox as the only authority would miss native behavior.

1. `0x424E00` reads `[APOption] RePartition`, default `0`, into dialog DWORD 43 and checkbox control 1018 (`ini-options`). The UTF-16 option name is at VA `0x62BCD0`, file offset `0x22A4D0`; this byte location was checked directly in the executable. Provider string search did not enumerate this literal, so its omission is not evidence of absence.
2. `0x41EE20` opens a **raw PIT/XML file picker**, puts the selected path into control 1041, and checks control 1018 (`manual-pit-picker`). The XML filter alone does not establish XML conversion support in the examined upload path.
3. `0x41E860` reads dialog state and builds the request: `request[3]=dialog[43]`, `request[8]=(dialog[43]!=0)`, and the raw PIT path starts at `request[665]` (`ui-start-request`). `0x429150` copies that request into each engine through `0x42E3C0`, whose `memcpy` starts at engine byte offset `+24` (`start-download-options`, `request-copy`). Consequently, engine `+36` enables PIT upload, `+56` selects the explicit external PIT, and `+2684` holds that path. These are recovered object offsets, not ABI field names.
4. While scanning an archive, the PIT member branches in **both** `0x42EF40` and `0x430460` set engine `+6040=1` **and `+36=1`**, retain the archive path at `+6044`, member length at `+6564` and member offset at `+6568` (`tar-parser`, `tar-member-selector`). For example, `0x430E3F` sets the package-PIT flag and `0x430E4F` enables upload. The string comparison helper `0x4278E0` returns a case-insensitive comparison result, confirming that these are the equality branches for `PIT` (`string-compare`).
5. `0x435240` returns immediately if engine `+36` is zero. Otherwise, in ordinary archive mode, it uses the raw external file when no package PIT was found **or** `+56` is set. Otherwise it reads exactly the retained PIT member from its archive. The separate one-click-container branch reads its recorded PIT offset/length (`pit-upload`).

**Observed consequence:** a discovered package PIT can enable upload even when the initial INI setting was `RePartition=0`. This is a host-code observation, not a claim that every firmware archive contains a PIT or that every bootloader changes its layout. The native tab warning in `0x427310` also says the device will be changed with the PIT in the CSC file (`pit-tab-warning`). The port's metadata-only reference behavior is therefore materially different from native package-PIT processing.

Conversely, in ordinary archive mode, checking Re-Partition forces the explicit-file branch: an embedded PIT does not rescue an empty or unreadable manual PIT path. The examined routine attempts to open that external path and fails the stage if it cannot (`ui-start-request`, `pit-upload`). This is a native input failure path, separate from OdinBCC's missing upload support.

## Native Odin: execution order and wire sequence

`0x42E470` constructs this normal eight-stage sequence (`download-stages`):

```text
0x434170  file analysis / archive PIT discovery
0x434D50  ODIN/LOKE handshake
0x434FB0  session negotiation
0x435AD0  DEVINFO stage
0x435240  conditional PIT UPLOAD
0x435750  optional erase, then device PIT DOWNLOAD and route mapping
0x435DE0  firmware image transfers
0x436180  close / reboot
```

Its alternate four-stage branch omits file analysis, PIT upload/download and image transfers; the eight-stage branch stops when a stage returns false. The optional erase in `0x435750` uses separate command `0x64/7` under engine flag `+52`. Repartition must not be conflated with that erase operation.

The upload is a PIT-specific flow, not an ordinary image transfer. All command/subcommand values below are **hexadecimal**; payload words are little-endian DWORDs after those two fields.

| Stage | Host request | Response handling observed in this binary |
| --- | --- | --- |
| Enter PIT upload | `0x65/0`, no payload | Standard 8-byte control reply |
| Announce length | `0x65/2`, payload `[pit_bytes]` | Standard 8-byte control reply |
| Send PIT | One raw `pit_bytes` buffer through `0x4326C0` | Reads 8 bytes after the raw send |
| Finalize PIT | `0x65/3`, payload `[pit_bytes]` | Standard 8-byte control reply; sentinel failure stops the stage |
| Read resulting device layout | `0x65/1`, then `0x65/2 [block_index]`, then `0x65/3` without a payload | Size, raw blocks of up to 500 bytes, final control reply |
| Map firmware | `0x432C10` / `0x4329E0` | Parses returned PIT and fills image routes by filename before image transfer |

Evidence labels: `pit-upload`, `pit-data-sender`, `control-command`, `pit-download`, `device-pit-parser`, `route-mapper`.

`0x4324D0` constructs a zeroed **1024-byte control buffer** with command at DWORD 0, subcommand at DWORD 1 and payload at DWORD 2. For ordinary control replies it checks that the first response word equals the command and returns the second word; I/O/response-ID failure returns `0x80000000` (`control-command`). The raw PIT sender passes the exact PIT length through `0x437AB0` to `WriteFile` in `0x438300` (`serial-write`, `write-file`). There is no 500-byte indexed upload loop in this inspected path; those indexed blocks belong to PIT **download**. This describes host writes, not physical USB packet sizes or driver segmentation.

The downloaded PIT is parsed as a 28-byte header plus 132-byte entries. Native mapping fills unresolved image descriptors from returned PIT entries by filename (`device-pit-parser`, `route-mapper`). Native Odin does not require the *old* live PIT to be byte-identical to the candidate PIT before this upload stage. The current port's equality guard cannot simply be moved out of the way without supplying the missing state transition and validating the resulting layout.

## Native error-handling weakness

Native Odin is a protocol reference, not a model for every error check. `0x4326C0` returns a transfer success boolean, but the upload caller at `0x435694` **does not test it** before freeing the buffer and submitting finalization. Assembly confirms the next instructions at `0x435699` onward consume the buffer pointer, not the return value (`pit-upload-assembly`). Also, `0x4326C0` reads the 8-byte data reply but does not validate its response ID or status words (`pit-data-sender-assembly`).

These are concrete weaknesses in the inspected native path. A future port should check exact writes, data acknowledgements and finalization, abort on failure, and avoid replay after a PIT side effect. Static analysis cannot establish how a particular bootloader responds to a truncated or rejected PIT or whether its final reply reliably detects every earlier failure.

## Work required for actual support

This is a proposed implementation boundary, not work included in this report:

1. Add an explicit, versioned repartition request and distinguish its writable candidate PIT from today's metadata-only reference. Preserve the existing reference contract. Do not silently repurpose the currently rejected bit 2 without reviewing ABI/version compatibility.
2. Accept and validate the intended PIT source, define external-versus-package precedence, hold its source identity through the operation, and validate the candidate layout and planned images before any PIT write. Keep manufacturer identity requirements for the negotiated protocol; do not invent an authentication bypass. Define data policy explicitly for a layout-changing request.
3. Add a transport-reviewed upload stage after negotiation/required identity exchange and before image mapping. Follow the verified `65/0 -> 65/2 [size] -> raw PIT -> 65/3 [size]` sequence, with stricter error handling than the native weakness above. Read back the resulting device PIT and rebuild/revalidate image routes and capacities before image writes.
4. Keep ordinary WRITE's current-layout checks. In repartition mode, compare the read-back result with the intended candidate rather than requiring the old layout to equal it. Account for PIT-only irreversible side effects in progress/results even when zero firmware images complete. A PIT write failure must not become a generic retry or claim that nothing was written.
5. Add independent fake-transport fixtures for exact command bytes/order, raw PIT lengths, every failed/short/foreign reply, cancellation before and after the PIT side effect, read-back mismatch, changed route/capacity, explicit versus package PIT, and unchanged reference-only writes. Validate stock-driver and WinUSB behavior separately; this static stock-driver trace does not establish WinUSB repartition compatibility.

## Verification and remaining limits

Fresh verification on the source baseline:

```powershell
py build.py --objects-only --configuration Debug
py tests/native_tests.py --configuration Debug --target native_streaming_tests --target native_session_tests
```

- Build: 16 classic BCC32 objects compiled, exit 0. Existing constant-condition warning in `Odin4FlashOperation.cpp:177` and existing LZ4/xxhash warnings remain; no compiler errors. Test targets link 15 engine/LZ4 objects, excluding the GUI object.
- Streaming: **841 checks passed**, fresh compile/run, exit 0.
- Session/fake transports: **11,405,308 checks passed**, fresh compile/run, exit 0.
- Existing reference-image exclusion regressions: [streaming tests](../tests/native_streaming_tests.cpp#L53). Existing logical-PIT mismatch and removed-bit-2 rejection across WRITE/READ_PIT/PLAN_ONLY: [session tests](../tests/native_session_tests.cpp#L548); rejected options issue zero device commands.
- Evidence selection was checked against the full REA export; REA's `inspect-evidence-bundle` accepted all 22 records after semantic-ID and canonical-manifest validation. Every retained record binds to the target SHA-256. All 17 relative report links and line anchors resolve; `git diff --check` passed. The original target hash remained unchanged and the owned session closed. An independent read-only agent review found no factual issues in the report's source/native comparisons.

Those passing tests validate the **current non-repartition behavior**. There are no repartition success/failure tests in the current port. Release, full GUI validation and physical flashing were not rerun for this documentation change. No source cleanup or simplification was made; lint/typecheck/static-analysis checks beyond compilation and documentation checks were not run.

The IDA adapter does not establish complete function-body ranges, CFG completeness, every indirect call target or original source types. Analysis-profile digest is retained in the index. Device-side PIT validation, partition migration/erase semantics, power-loss recovery, XML conversion, and compatibility across all models, storage types and bootloader versions remain unverified. Nothing in this report demonstrates a successful physical repartition or a universal recovery method.
