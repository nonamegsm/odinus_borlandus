"""Hardware-free tests for the streaming HHD Device Monitoring Studio parser."""
import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import tracemalloc
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PARSER = ROOT / "tools" / "hhd_usb_trace.py"


def event(number, direction, transfer=None, length=None, endpoint=1, status="0x0", kind="Bulk or Interrupt Transfer"):
    pipe = (f"Pipe Handle: 0x1234 (Endpoint Address: 0x{endpoint:x})" if endpoint is not None
            else "Pipe Handle: Control Pipe")
    data = "" if transfer is None else f"\r\n{transfer} 0x{length:x} bytes to the device"
    return (f'<p class="st07"><span class="st00">{number:06d}: {kind} ({direction}), '
            '2026-10-29 15:09:59.496502 +0.110217. '
            f'(1. Device: Samsung &amp; Test) Status: {status}\r\n</span>{pipe}{data}</p>\r\n')


def table(data, ascii_text="ASCII 12 34 AB CD"):
    # Data and printable columns are separate, as in the real HHD export.
    lines = [" ".join(f"{value:02X}" for value in data[i:i + 16]) for i in range(0, len(data), 16)]
    return ('<table width="100%"><tr><td valign="top" class="st08"><pre> '
            + "\r\n ".join(lines)
            + '</pre></td><td valign="top" class="st08"><pre>' + ascii_text
            + '</pre></td></tr></table>\r\n')


def document(*parts):
    return ('<!DOCTYPE html><html><body><!--StartFragment --><pre>\n' + "".join(parts)
            + '</pre><!--EndFragment --></body></html>\n').encode("utf-8")


def run_cli(path, *options):
    return subprocess.run([sys.executable, str(PARSER), str(path), *map(str, options)],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                          encoding="utf-8", timeout=30)


class HhdUsbTraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write_capture(self, data):
        path = self.root / "capture.html"
        path.write_bytes(data)
        return path

    @staticmethod
    def rows(result):
        return [json.loads(line) for line in result.stdout.splitlines() if line]

    def test_events_and_exact_ack_do_not_include_ascii_or_next_event(self):
        command = bytes.fromhex("660000000600000033f13f010000e001") + bytes(1008)
        ack = bytes.fromhex("6400000000800300")
        source = self.write_capture(document(
            event(100, "DOWN", "Send", 4) + table(b"ODIN", "4F 44 49 4E 00 FF"),
            event(101, "UP", "Send", 4),
            event(102, "UP", "Get", 4, 0x81) + table(b"LOKE", "12 34 56 78"),
            event(103, "DOWN", "Get", 4096, 0x81, "-0x3fff0000"),
            event(104, "DOWN", "Send", 1024) + table(command),
            event(105, "UP", "Send", 1024),
            event(106, "DOWN", "Send", 0),
            event(107, "UP", "Get", 8, 0x81) + table(ack, "FF EE DD CC BB AA 99 88"),
            event(108, "DOWN", "Get", 4096, 0x81),
            event(109, "DOWN", "Send", 4) + table(b"NEXT"),
            event(110, "UP", endpoint=None, kind="Vendor-Specific Request"),
        ))
        result = run_cli(source, "--prefix-bytes", "16", "--odin")
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows(result)
        self.assertEqual([r["number"] for r in rows], list(range(100, 111)))
        first = rows[0]
        self.assertEqual((first["direction"], first["endpoint"], first["transfer"], first["length"]),
                         ("DOWN", 1, "send", 4))
        self.assertEqual(first["timestamp"], "2026-10-29 15:09:59.496502")
        self.assertEqual(str(first["delta_seconds"]), "0.110217")
        self.assertEqual(first["status"], "0x0")
        self.assertIn("Samsung & Test", first["description"])
        self.assertEqual(first["data_hex"].lower(), b"ODIN".hex())
        self.assertEqual(rows[2]["data_hex"].lower(), b"LOKE".hex())
        self.assertEqual(rows[3]["status"], "-0x3fff0000")
        self.assertNotIn("data_error", rows[3])  # pending IN read has no bytes yet
        self.assertEqual(rows[4]["data_hex"].lower(), command[:16].hex())
        self.assertTrue(rows[4]["data_truncated"])
        self.assertEqual(rows[4]["odin"]["candidate"], "command")
        self.assertEqual((rows[4]["odin"]["command"], rows[4]["odin"]["subcommand"]), (0x66, 6))
        self.assertEqual(rows[4]["odin"]["words"][:4], [0x66, 6, 0x013ff133, 0x01e00000])
        self.assertEqual(rows[7]["data_hex"].lower(), ack.hex())
        self.assertEqual(len(bytes.fromhex(rows[7]["data_hex"])), 8)
        self.assertEqual(rows[7]["odin"]["candidate"], "response")
        self.assertEqual((rows[7]["odin"]["id"], rows[7]["odin"]["ack"]), (0x64, 0x00038000))
        self.assertFalse(rows[7].get("data_truncated", False))
        self.assertNotIn("data_error", rows[6])  # 0-byte transfer is not missing data
        self.assertEqual(rows[9]["data_hex"].lower(), b"NEXT".hex())
        self.assertEqual(rows[10]["kind"], "Vendor-Specific Request")

    def test_missing_or_short_ack_never_consumes_next_table(self):
        good = bytes.fromhex("6600000000000000")
        source = self.write_capture(document(
            event(20, "UP", "Get", 8, 0x81),  # Missing own table, next event has one.
            event(21, "UP", "Get", 8, 0x81) + table(good, "DE AD BE EF"),
            event(22, "UP", "Get", 8, 0x81) + table(b"\x01\x02\x03", "00 00 00 00 00"),
            event(23, "UP", "Get", 8, 0x81) + table(good),
        ))
        result = run_cli(source, "--prefix-bytes", "16", "--odin")
        rows = self.rows(result)
        self.assertEqual([r["number"] for r in rows], [20, 21, 22, 23])
        self.assertTrue(rows[0].get("data_error"))
        self.assertNotEqual(rows[0].get("data_hex", "").lower(), good.hex())
        self.assertEqual(rows[1]["data_hex"].lower(), good.hex())
        self.assertTrue(rows[2].get("data_error"))
        self.assertNotIn("odin", rows[2], "a partial response is never a complete Odin response")
        self.assertNotEqual(rows[2].get("data_hex", "").lower(), good.hex())
        self.assertEqual(rows[3]["data_hex"].lower(), good.hex())
        self.assertNotEqual(result.returncode, 0, "incomplete captures must be visibly reported")

    def test_pending_transfer_without_table_is_not_data_corruption(self):
        source = self.write_capture(document(
            event(98, "DOWN", "Send", 10, None, "-0x3fff0000"),
            event(99, "DOWN", "Get", 4096, 0x81, "-0x3fff0000"),
            event(100, "DOWN", "Send", 4) + table(b"ODIN"),
        ))
        result = run_cli(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = self.rows(result)
        self.assertTrue(rows[0].get("data_unavailable"))
        self.assertNotIn("data_error", rows[0])
        self.assertNotIn("data_error", rows[1])
        self.assertEqual(rows[2]["data_hex"].lower(), b"ODIN".hex())

    def test_filter_and_output_file(self):
        source = self.write_capture(document(
            event(10, "DOWN", "Send", 4) + table(b"ODIN"),
            event(11, "UP", "Get", 4, 0x81) + table(b"LOKE"),
            event(12, "UP", "Get", 8, 0x81) + table(bytes.fromhex("6600000000000000")),
        ))
        output = self.root / "events.jsonl"
        result = run_cli(source, "--start-event", "11", "--end-event", "11", "--endpoint", "0x81", "-o", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")
        rows = [json.loads(line) for line in output.read_text(encoding="utf-8").splitlines()]
        self.assertEqual([r["number"] for r in rows], [11])
        self.assertEqual(rows[0]["data_hex"].lower(), b"LOKE".hex())

    def test_odin_negative_ack_and_explicit_metadata_only(self):
        failure = bytes.fromhex("ffffffff86ffffff")
        source = self.write_capture(document(event(1, "UP", "Get", 8, 0x81) + table(failure)))
        result = run_cli(source, "--prefix-bytes", "8", "--odin")
        self.assertEqual(result.returncode, 0, result.stderr)
        row = self.rows(result)[0]
        self.assertEqual(row["data_hex"].lower(), failure.hex())
        self.assertEqual(row["odin"]["id"], 0xffffffff)
        self.assertEqual(row["odin"]["ack"], 0xffffff86)
        self.assertEqual(row["odin"]["ack_signed"], -122)
        metadata = run_cli(source, "--prefix-bytes", "0")
        self.assertEqual(metadata.returncode, 0, metadata.stderr)
        self.assertNotIn("data_hex", self.rows(metadata)[0])
        bad = run_cli(source, "--prefix-bytes", "0", "--odin")
        self.assertNotEqual(bad.returncode, 0)

    def test_invalid_hex_never_becomes_a_valid_response(self):
        source = self.write_capture(document(
            event(2, "UP", "Get", 8, 0x81) +
            '<table><tr><td><pre>66 00 00 0G 00 00 00 00</pre></td>'
            '<td><pre>66 00 00 00 00 00 00 00</pre></td></tr></table>',
        ))
        result = run_cli(source, "--odin")
        self.assertNotEqual(result.returncode, 0)
        row = self.rows(result)[0]
        self.assertTrue(row.get("data_error"))
        self.assertNotIn("odin", row)

    def test_truncated_document_returns_failure(self):
        source = self.write_capture(b'<!DOCTYPE html><html><body><pre>' + event(1, "DOWN", "Send", 4).encode()
                                    + b'<table><tr><td><pre> 4F 44')
        result = run_cli(source, "--prefix-bytes", "16")
        self.assertNotEqual(result.returncode, 0, "truncated HTML must not be silently accepted")

    def test_refuses_input_as_output_and_hardlink_alias(self):
        original = document(event(1, "DOWN", "Send", 4) + table(b"ODIN"))
        source = self.write_capture(original)
        result = run_cli(source, "-o", source)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(source.read_bytes(), original)
        alias = self.root / "hardlink.html"
        try:
            os.link(source, alias)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"hardlinks unavailable: {exc}")
        result = run_cli(source, "-o", alias)
        self.assertNotEqual(result.returncode, 0, "hardlinked output must be refused")
        self.assertEqual(source.read_bytes(), original, "output alias must not truncate capture")

    def test_refuses_symlink_alias_if_available(self):
        original = document(event(1, "DOWN", "Send", 4) + table(b"ODIN"))
        source = self.write_capture(original)
        alias = self.root / "symlink.html"
        try:
            alias.symlink_to(source)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"symlinks unavailable: {exc}")
        result = run_cli(source, "-o", alias)
        self.assertNotEqual(result.returncode, 0, "symlink output must be refused")
        self.assertEqual(source.read_bytes(), original)

    def test_uses_bounded_reads_and_memory_for_large_single_line(self):
        spec = importlib.util.spec_from_file_location("hhd_usb_trace_under_test", PARSER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        payload_length = 3 * 1024 * 1024
        html = document(event(77, "DOWN", "Send", payload_length) +
                        '<table><tr><td><pre>' + ("00 " * payload_length) +
                        '</pre></td><td><pre>FA CE AA BB</pre></td></tr></table>')

        class GuardedStream(io.BytesIO):
            def read(self, size=-1):
                if size < 0 or size > 65536:
                    raise AssertionError(f"unbounded read: {size}")
                return super().read(size)

        stream = GuardedStream(html)
        tracemalloc.start()
        try:
            rows = list(module.iter_events(stream, prefix_bytes=32, chunk_size=4096))
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["data_hex"].lower(), "00" * 32)
        self.assertTrue(rows[0]["data_truncated"])
        self.assertLess(peak, 3 * 1024 * 1024, f"parser buffered the large data row: {peak} bytes")


if __name__ == "__main__":
    unittest.main()
