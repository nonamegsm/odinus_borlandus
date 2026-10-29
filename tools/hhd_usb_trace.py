#!/usr/bin/env python3
"""Stream an HHD Device Monitoring Studio HTML export as JSON Lines.

Each record describes one exported request or completion; DOWN/UP are HHD
phases, not USB endpoint direction.  Large payloads are *not* reconstructed:
only a bounded prefix from the hex column is retained.  Original files are
read-only and may be much larger than RAM.

Examples:
  python tools/hhd_usb_trace.py capture.html -o events.jsonl --odin
  python tools/hhd_usb_trace.py capture.html --start-event 1300 --end-event 1800
  python tools/hhd_usb_trace.py capture.html --endpoint 0x81 --prefix-bytes 8

Odin decoding is optional and labels candidates; an eight-byte response alone
does not prove which command it belongs to.  Payload data and identifiers can
contain sensitive device information; protect the resulting JSONL accordingly.
"""
import argparse
import html
import json
import os
import re
import struct
import sys
from pathlib import Path

_MARKER = b'<p '
_END_P = b'</p>'
_MAX_HEADER = 65536
_MAX_TABLE_LEAD = 65536
_EVENT = re.compile(
    r'(?P<number>\d+)\s*:\s*(?P<kind>.*?)\s*\((?P<direction>DOWN|UP)\)\s*,\s*'
    r'(?P<timestamp>.*?)\s*\+(?P<delta>\d+(?:\.\d+)?)\s*\.\s*'
    r'(?P<description>.*?)\bStatus\s*:\s*(?P<status>[^\r\n<]+)', re.I | re.S)
_ENDPOINT = re.compile(r'Endpoint\s+Address\s*:\s*(0x[0-9a-f]+|\d+)', re.I)
_TRANSFER = re.compile(r'\b(Send|Get)\s+(0x[0-9a-f]+|\d+)\s+bytes\b', re.I)
_TAGS = re.compile(r'<[^>]*>')
_HEX_CELL = re.compile(
    rb'<table\b[^>]*>\s*<tr\b[^>]*>\s*<td\b[^>]*>\s*<pre\b[^>]*>', re.I)
_HEX = frozenset(b'0123456789abcdefABCDEF')
_SPACE = frozenset(b' \t\r\n\f')


class _Event:
    def __init__(self, offset, prefix_bytes):
        self.offset = offset
        self.prefix_bytes = prefix_bytes
        self.header = bytearray()
        self.table_lead = bytearray()
        self.hex_text = bytearray()
        self.record = None
        self.state = 'header'
        self.want = 0

    def feed(self, data):
        if not data or self.state == 'done':
            return
        if self.state == 'header':
            # Headers are tiny even when the following table is gigabytes.
            room = _MAX_HEADER + len(_END_P) - len(self.header)
            self.header.extend(data[:max(0, room)])
            end = self.header.find(_END_P)
            if end < 0:
                if len(self.header) >= _MAX_HEADER:
                    self.record = {'offset': self.offset, 'parse_error': 'event header exceeds limit or is unterminated'}
                    self.state = 'done'
                return
            consumed = end + len(_END_P)
            header = bytes(self.header[:consumed]).decode('utf-8', 'replace')
            text = html.unescape(_TAGS.sub('', header))
            match = _EVENT.search(text)
            if not match:
                self.state = 'done'
                if re.search(r'\b\d+\s*:', text):
                    self.record = {'offset': self.offset, 'parse_error': 'unrecognized HHD event header'}
                return
            self.record = {
                'number': int(match.group('number')),
                'kind': match.group('kind').strip(),
                'direction': match.group('direction').upper(),
                'timestamp': match.group('timestamp').strip(),
                'delta_seconds': match.group('delta'),
                'status': match.group('status').strip(),
                'offset': self.offset,
            }
            description = match.group('description').strip()
            if description:
                self.record['description'] = description
            endpoint = _ENDPOINT.search(text)
            if endpoint:
                self.record['endpoint'] = int(endpoint.group(1), 0)
            transfer = _TRANSFER.search(text)
            if transfer:
                self.record['transfer'] = transfer.group(1).lower()
                self.record['length'] = int(transfer.group(2), 0)
            remaining = bytes(self.header[consumed:])
            # If the input chunk was longer than the header cap, its remainder
            # has not yet been copied into the bounded header buffer.
            copied = min(len(data), max(0, room))
            remaining += data[copied:]
            self.header.clear()
            length = self.record.get('length', 0)
            transfer = self.record.get('transfer')
            direction = self.record['direction']
            self.want = min(length, self.prefix_bytes)
            carries_data = ((transfer == 'send' and direction == 'DOWN') or
                            (transfer == 'get' and direction == 'UP'))
            if not self.want or not carries_data:
                self.state = 'done'
                return
            self.state = 'table'
            self.feed(remaining)
            return
        if self.state == 'table':
            room = _MAX_TABLE_LEAD - len(self.table_lead)
            self.table_lead.extend(data[:max(0, room)])
            match = _HEX_CELL.search(self.table_lead)
            if match:
                remaining = bytes(self.table_lead[match.end():])
                copied = min(len(data), max(0, room))
                remaining += data[copied:]
                self.table_lead.clear()
                self.state = 'hex'
                self.feed(remaining)
            elif len(self.table_lead) >= _MAX_TABLE_LEAD:
                self.record['data_error'] = 'hex data table missing or exceeds search limit'
                self.state = 'done'
            return
        if self.state == 'hex':
            # A hex prefix is bounded independently of the data transfer size.
            # Allow generous whitespace but never accumulate an unbounded table.
            cap = self.want * 4 + 8192
            room = cap - len(self.hex_text)
            self.hex_text.extend(data[:max(0, room)])
            end = self.hex_text.find(b'</pre>')
            if end >= 0 or len(self.hex_text) >= cap:
                self._finish_hex(end)

    def _finish_hex(self, end):
        body = self.hex_text if end < 0 else self.hex_text[:end]
        result = bytearray()
        i = 0
        while i < len(body) and len(result) < self.want:
            if body[i] in _SPACE:
                i += 1
                continue
            if i + 1 >= len(body):
                break
            if body[i] not in _HEX or body[i + 1] not in _HEX or (i + 2 < len(body) and body[i + 2] not in _SPACE):
                self.record['data_error'] = 'malformed hex data column'
                break
            result.append(int(bytes(body[i:i + 2]), 16))
            i += 2
        self.record['data_hex'] = result.hex()
        if len(result) < self.want and 'data_error' not in self.record:
            self.record['data_error'] = 'hex data column is truncated'
        if len(result) == self.want and self.want < self.record['length']:
            self.record['data_truncated'] = True
        self.hex_text.clear()
        self.state = 'done'

    def finish(self):
        if self.state == 'header':
            if re.search(rb'\b\d+\s*:', self.header):
                self.record = {'offset': self.offset, 'parse_error': 'event header is truncated'}
        elif self.state == 'table':
            if self.record.get('status', '').lower() in ('0', '0x0', '0x00000000'):
                self.record['data_error'] = 'hex data table not present in event'
            else:
                # Failed/pending transfers may not have a data payload to export.
                self.record['data_unavailable'] = 'transfer did not provide a hex data table'
        elif self.state == 'hex':
            self._finish_hex(self.hex_text.find(b'</pre>'))
        return self.record


def iter_events(stream, prefix_bytes=64, chunk_size=1048576):
    """Yield HHD event dicts from a binary stream, using bounded reads.

    `offset` is a byte offset in the original export. `data_hex` comes only
    from the first (hex) table cell and never exceeds the transfer length.
    """
    if prefix_bytes < 0 or prefix_bytes > 65536 or chunk_size < 1:
        raise ValueError('invalid prefix or chunk size')
    carry = b''
    base = 0
    event = None
    while True:
        block = stream.read(chunk_size)
        if not block:
            break
        data = carry + block
        safe = max(0, len(data) - len(_MARKER) + 1)
        position = 0
        while True:
            marker = data.find(_MARKER, position)
            if marker < 0 or marker >= safe:
                break
            if event is not None:
                event.feed(data[position:marker])
                record = event.finish()
                if record is not None:
                    yield record
            event = _Event(base + marker, prefix_bytes)
            event.feed(_MARKER)
            position = marker + len(_MARKER)
        safe = max(safe, position)
        if event is not None:
            event.feed(data[position:safe])
        base += safe
        carry = data[safe:]
    if event is not None:
        event.feed(carry)
        record = event.finish()
        if record is not None:
            yield record


def _add_odin(record):
    raw = bytes.fromhex(record.get('data_hex', ''))
    if record.get('data_error') or len(raw) < 8:
        return
    direction, transfer, length = record.get('direction'), record.get('transfer'), record.get('length')
    if direction == 'DOWN' and transfer == 'send' and length == 1024:
        words = [x[0] for x in struct.iter_unpack('<I', raw[:min(len(raw) // 4 * 4, 44)])]
        if words[0] in range(0x64, 0x6a):
            record['odin'] = {'candidate': 'command', 'command': words[0], 'subcommand': words[1], 'words': words}
    elif direction == 'UP' and transfer == 'get' and length == 8:
        response_id, ack = struct.unpack('<II', raw[:8])
        record['odin'] = {'candidate': 'response', 'id': response_id, 'ack': ack,
                          'ack_signed': ack if ack < 0x80000000 else ack - 0x100000000}


class _TrackingReader:
    def __init__(self, stream):
        self.stream = stream
        self.tail = b''

    def read(self, size):
        data = self.stream.read(size)
        self.tail = data[-256:] if len(data) >= 256 else (self.tail + data)[-256:]
        return data


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('input', type=Path, help='HHD HTML export (read only)')
    parser.add_argument('-o', '--output', type=Path, help='JSONL output file; default stdout')
    parser.add_argument('--prefix-bytes', type=int, default=64, metavar='N', help='capture at most N payload bytes per event (0..65536; default 64)')
    parser.add_argument('--start-event', type=int, help='include events with numbers at or above N')
    parser.add_argument('--end-event', type=int, help='include events with numbers at or below N')
    parser.add_argument('--endpoint', type=lambda value: int(value, 0), help='include only this endpoint, e.g. 0x81')
    parser.add_argument('--odin', action='store_true', help='interpret eligible payload prefixes as candidate Odin commands or responses')
    args = parser.parse_args(argv)
    if not 0 <= args.prefix_bytes <= 65536:
        parser.error('--prefix-bytes must be in 0..65536')
    if args.odin and args.prefix_bytes < 8:
        parser.error('--odin needs at least 8 prefix bytes')
    if args.start_event is not None and args.end_event is not None and args.start_event > args.end_event:
        parser.error('--start-event must not exceed --end-event')
    if args.output:
        if args.input.resolve() == args.output.resolve():
            parser.error('output must not overwrite the capture')
        try:
            if args.output.exists() and os.path.samefile(args.input, args.output):
                parser.error('output must not overwrite the capture via a hard link')
        except OSError as error:
            parser.error('cannot verify input/output identity: ' + str(error))
    count = errors = written = 0
    try:
        with args.input.open('rb') as source:
            reader = _TrackingReader(source)
            if args.output:
                # Open without truncation, then compare the opened handles.
                # This also protects against a link being changed after the
                # earlier path-level check and before the destination opens.
                fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | getattr(os, 'O_BINARY', 0), 0o666)
                try:
                    if os.path.samestat(os.fstat(source.fileno()), os.fstat(fd)):
                        raise ValueError('output resolves to the input capture')
                    destination = os.fdopen(fd, 'w', encoding='utf-8', newline='\n')
                    fd = -1
                    destination.seek(0)
                    destination.truncate(0)
                finally:
                    if fd >= 0:
                        os.close(fd)
            else:
                destination = sys.stdout
            try:
                for record in iter_events(reader, args.prefix_bytes):
                    count += 1
                    if 'parse_error' in record or 'data_error' in record:
                        errors += 1
                    number = record.get('number')
                    if number is not None and ((args.start_event is not None and number < args.start_event) or
                                               (args.end_event is not None and number > args.end_event)):
                        continue
                    if args.endpoint is not None and record.get('endpoint') != args.endpoint:
                        continue
                    if args.odin:
                        _add_odin(record)
                    destination.write(json.dumps(record, ensure_ascii=True, separators=(',', ':')) + '\n')
                    written += 1
                if b'</html>' not in reader.tail.lower():
                    errors += 1
                    print('HHD parse: missing closing </html>; capture may be truncated', file=sys.stderr)
                if not count:
                    errors += 1
                    print('HHD parse: no events found', file=sys.stderr)
            finally:
                if args.output:
                    destination.close()
    except (OSError, ValueError) as error:
        print('HHD parse: ' + str(error), file=sys.stderr)
        return 2
    print('HHD parse: {} events, {} emitted, {} errors'.format(count, written, errors), file=sys.stderr)
    return 2 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
