# Native Samsung Download protocol compatibility

This implementation selects the supported wire flow from the negotiated protocol and capability word. It does not use a phone-model, firmware-build or image-hash eligibility list.

| Transport | Supported flow | Write prerequisites |
| --- | --- | --- |
| Samsung stock modem driver | Native protocol 1 | Raw 128 KiB packets; validated firmware and live PIT capacity |
| Samsung stock modem driver | Native protocols 2–3 | Negotiated 1 MiB packets; raw fallback, or native LZ4 block records when protocol >=3 advertises low-word capability 0x8000 |
| Samsung stock modem driver | Native protocols 4–5 | DEVINFO exchange and authenticated package model/carrier policy, then raw or negotiated encoded transfer |
| WinUSB | Reviewed protocols 1–3 | Existing pinned raw framing retained |

Version 0, unknown versions and unreviewed WinUSB 4/5 flows remain rejected. MMC and UFS capacity validation still uses 512-byte and 4096-byte PIT units per selected partition. Other addressing schemes and unmeasured zero-count USERDATA remain unsupported. A validated package PIT is still required. These limits are separate from protocol negotiation.

## Encoded transfer

Native Odin negotiates compression using protocol >=3 plus low-word bit 0x8000. Each sequence sends 66/5, 66/6 with stored-record and decoded lengths, indexed padded packets, and 66/7 with exact decoded length, PIT route and last-sequence marker. The port sends original LZ4 block records, excluding frame headers and footers. Independent 64/256/1024 KiB blocks with declared content size are supported; valid unsupported frame profiles select host decoding before writes.

Every encoded reader is audited over the same held archive before connecting. Frame/block/content checksums and decoded SHA256 remain enforced. A final sequence is locally verified before submission. Progress credits decoded bytes after the sequence-completion ACK; compressed packet lengths are not misreported as decoded bytes. Negative/partial ACKs, cancellation and reader failures stop the session. There is no mid-image fallback or automatic replay after writes start.

## DEVINFO and signed firmware identity

Native 4/5 uses 69/0 for size, indexed 69/1 requests followed by raw 500-byte blocks, then 69/2. The parser bounds the magic/count/tag table, offsets and terminated model/carrier strings. The unused third record word remains opaque. Version 4 needs a model; version 5 also needs carrier tag 3. An explicitly present empty carrier is accepted, matching the native validator.

A write requires ordinary signed meta-data/*.mf metadata in each selected image archive. The 256-byte signature prefix authenticates the exact text payload with RSA-2048 PKCS#1 v1.5/SHA-256 and only the two manufacturer public keys observed in native Odin. No caller-supplied trust key or authentication bypass is exposed. Each non-metadata TAR member is checked against its signed SHA256 over stored bytes, including the LZ4 envelope. All selected archives must authenticate the same model; the native carrier block list remains enforced. This is a comparison against the selected authenticated package, not a fixed allowed-model list.

Read-only DEVINFO/PIT inspection does not require firmware identity, and cannot later gain write authority merely by announcing a transfer total. Missing, ambiguous, malformed or unauthenticated identity is rejected before any image command. The supplied S24 firmware has no .mf members and remains usable on its reported protocol 3; its filenames are never used to invent identity for protocol 4/5.

## Evidence and verification boundary

The inspected artifact is F:\Firmware_Test\Odin3_v3.14.4\Odin3_v3.14.4.exe, SHA256 aa17ccf37d52c816bcea6e54dbdd6daf90fe2ac984641d01034ade77ff9dcb41. REA 6.4.0 used owned headless IDA sessions through the existing pinned compatible adapter. The target was not executed. Each owned session closed; full retained records are under the EasyJTAG workspace .omx/research/odin-universal-protocol and matching earlier records under the EasyJTAG workspace .omx/research/odin-native-rea.

Key REA function Evidence:

- Negotiation 434FB0: ev_18de09a2e2be55c80b903019bba4797c6455a934417c91e3d293bda7b802c645
- Compressed sender 433380: ev_3c1bcf541e96edfc3a44b0c43ddaecf9a3f298326887ec8160771db3e5484614
- DEVINFO retrieval 435AD0: ev_e51a03256aaa923967bb8fba340335f1fcef0bb2586308acb07d1f5e132d32ad
- DEVINFO policy 432EE0: ev_517fb34597efff0a90ffc62b31c48822e37347f2af3c0eadc81eacd58acf9355
- Ordinary package identity setup 4365B0: ev_71097ee576f4e98aed3a17fc139666b3fe21ca3ad58cbd14d094442aee72dc40
- Signed member selector 40A280: ev_896cebedd61963aa92e1128be1b6d6d922d804a5531aac17489ed4c2112c480a
- Signed payload/parser 40A380: ev_9e980371a9c681beed95c6198e34750a49f4d6d44d9761a4993390f91032cb70
- Manufacturer public keys 409D50: ev_b7ea0f662c678fe62f8f98029aa2ba99bc0c5c951a50b20110bc1d76c6ad01f7
- Signature filter 40BCA0: ev_08d9bb22e7c93d6c3214782a219e8183424da56f2cff87cc7af061f8458269fe
- Verifier constructor 4116A0 (PKCS1v15/SHA256 vtable binding): ev_b861735f8553ca2e40b3943e0de3726406902316e2530737b43bf2593990d00a
- Native close/reboot 436180: ev_f2fb5030f318d72f7508e87ef7aa6cde5a0b05781dd29aa1710bd84679f865e7

The native verifier constructor 4116A0 explicitly installs the RSASS<PKCS1v15,SHA256> verifier vtable; [Crypto++ filter flags](https://cryptopp.com/docs/ref800/filters_8h_source.html) establish flag 9 as signature-at-begin plus result output. The existing Windows Advapi32 implementation reverses big-endian signature bytes as required by [CryptVerifySignature](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-cryptverifysignaturew). No new third-party dependency is introduced.

Static analysis establishes host flow and packet construction, not live bootloader behavior. Tests use fake transports, real archive planning and independent generated-key cryptographic vectors. No genuine manufacturer-signed positive manifest fixture was available, so complete positive authenticated-archive-to-protocol-4/5 execution and physical compatibility remain untested. The production verifier explicitly rejects the generated test key. No phone was enumerated, opened, read or flashed for this change.

ABI 1.12 preserves the existing exports and packed 64/537/68-byte request/progress/result records. It adds an explicit stock-driver [repartition operation](REPARTITION.md), optional raw target-PIT role, and independent PIT side-effect proof. Ordinary WRITE/READ_PIT and metadata-reference semantics remain unchanged. Native LZ4 record and DEVINFO handling remain required; the portable executable and matching manifest are published together.
