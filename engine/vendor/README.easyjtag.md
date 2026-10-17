# Odin4 source subset

Pinned upstream source is in upstream/; UPSTREAM.json records the exact commit, byte lengths and SHA256 values. Files came from git archive, so the preserved byte streams do not depend on Windows checkout newline conversion. Do not edit the snapshot silently; apply port changes in Addons/EasyJtag_Odin4 or record a deliberate upstream update.

LICENSE and NOTICE are preserved verbatim. This step includes only portable protocol/types, the upstream offline PIT fuzz utility, TAR MD5 trailer location and original offline tests. It does not contain or link libusb, Crypto++ or LZ4; their names remain in the unmodified upstream NOTICE/README as upstream context.

The local adapter is built by Addons/EasyJtag_Odin4/CMakeLists.txt. Full source remains available from the pinned repository revision for later USB/session/firmware port steps.

Git attributes preserve the original CRLF byte streams and the upstream README Markdown hard breaks. Source hashes still enforce the exact pinned content; local port code keeps the normal whitespace policy.
