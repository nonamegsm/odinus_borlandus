# HHD Device Monitoring Studio HTML exports

`tools/hhd_usb_trace.py` converts an HHD HTML export into one JSON object per
event without loading the capture or its full packet payloads into memory.
Python 3 is required; the parser has no third-party dependencies.

```powershell
python tools/hhd_usb_trace.py 'F:\captures\sniff.html' -o 'F:\captures\events.jsonl' --odin
```

Use `--start-event` and `--end-event` to limit the emitted event numbers,
`--endpoint 0x81` to filter an endpoint, and `--prefix-bytes` to control the
maximum payload prefix (default 64 bytes, maximum 65536). A zero prefix omits
payload bytes. The input is always read completely, even when filtering, so
truncated exports elsewhere in the file are still reported. The output path
must not refer to the input file. The command returns nonzero on parsing
errors; records can still be emitted before an error is found.

Each JSON line includes the event number, timestamp, HHD `DOWN`/`UP` phase,
status and source byte offset. Transfer records also include direction,
endpoint, size and any captured hex prefix. A prefix is marked truncated when
more payload exists. Missing or malformed data is reported instead of being
reconstructed from the adjacent printable-text column or a following event.
`--odin` interprets eligible 1024-byte requests and 8-byte responses as
**candidates**; it does not associate responses with requests or establish a
completed flash. HHD's `DOWN` and `UP` describe request and completion phases,
not USB endpoint direction.

Captures and their JSONL output can contain device identifiers and firmware
bytes. Store them appropriately and inspect the source capture when a complete
payload or protocol conclusion is required. The parser never opens a device.
