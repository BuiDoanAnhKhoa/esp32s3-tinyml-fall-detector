"""Host recorder tests, including real pyserial reads through a Linux PTY."""

import csv
import importlib.util
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "capture_mpu.py"
spec = importlib.util.spec_from_file_location("capture_mpu", SCRIPT)
capture = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = capture
spec.loader.exec_module(capture)


def record(sequence=1, timestamp=10000):
    return f"MPU1,{sequence},{timestamp},0.1,-0.2,1.0,2.0,-3.0,4.0\r\n".encode()


class ParserTests(unittest.TestCase):
    def test_units_and_logs(self):
        self.assertEqual(capture.parse_sample(record()), (1, 10000, 0.1, -0.2, 1, 2, -3, 4))
        self.assertIsNone(capture.parse_sample(b"\x1b[32mI (12) APP: ready\x1b[0m"))

    def test_invalid_samples(self):
        for line in (b"MPU1,1,2,3", record(-1), record(timestamp=-1),
                     record(2**32), record().replace(b"0.1", b"nan"),
                     record().replace(b"0.1", b"inf"), record().replace(b"0.1", b"\xff")):
            with self.subTest(line=line), self.assertRaises((ValueError, UnicodeError)):
                capture.parse_sample(line)

    def test_fragmented_and_oversized_lines_recover(self):
        lines = capture.LineBuffer()
        self.assertEqual(list(lines.feed(b"MPU")), [])
        result = list(lines.feed(record()[3:] + b"x" * 600))
        self.assertEqual(capture.parse_sample(result[0])[0], 1)
        self.assertEqual(lines.oversized, 1)
        self.assertEqual(len(lines.pending), 0)
        self.assertEqual(list(lines.feed(b"discard this\n" + record(2))), [record(2)[:-1]])

    def test_gaps_wrap_duplicates_and_restarts(self):
        stats = capture.Stats()
        for sequence, timestamp in ((0xFFFFFFFF, 10000), (0, 20000), (3, 50000),
                                    (3, 50000), (0, 1000), (1, 11000)):
            stats.observe((sequence, timestamp))
        self.assertEqual(stats.missing, 2)
        self.assertEqual(stats.duplicates, 1)
        self.assertEqual(stats.restarts, 1)
        self.assertEqual(stats.timing_gaps, 1)


@unittest.skipUnless(sys.platform.startswith("linux"), "PTY integration uses Linux")
class SerialCaptureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "data.csv"
        self.master, self.slave = os.openpty()
        self.addCleanup(self.close_ports)
        self.port = os.ttyname(self.slave)
        self.process = None
        self.addCleanup(self.stop_process)

    def close_ports(self):
        for fd in (self.master, self.slave):
            if fd is not None:
                os.close(fd)

    def stop_process(self):
        if self.process is not None:
            if self.process.poll() is None:
                self.process.kill()
            self.process.communicate()

    def start(self, *args):
        self.process = subprocess.Popen(
            [sys.executable, str(SCRIPT), "--port", self.port,
             "--output", str(self.output), *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        # Wait for the recorder to open/configure the PTY before sending bytes.
        ready = bytearray()
        while b"Waiting for MPU1 records" not in ready:
            readable, _, _ = select.select([self.process.stderr], [], [], 5)
            self.assertTrue(readable, "recorder did not start")
            chunk = os.read(self.process.stderr.fileno(), 4096)
            self.assertTrue(chunk, ready.decode())
            ready.extend(chunk)

    def finish(self):
        _, stderr = self.process.communicate(timeout=5)
        with self.output.open(newline="") as source:
            rows = list(csv.DictReader(source))
        return self.process.returncode, stderr.decode(), rows

    def test_real_serial_csv_capture(self):
        self.start("--duration", "0.4")
        os.write(self.master, b"boot log\nMPU")
        os.write(self.master, record()[3:] + record(3, 30000))
        os.write(self.master, b"MPU1,broken\n" + record(4, 40000))
        code, stderr, rows = self.finish()
        self.assertEqual(code, 0, stderr)
        self.assertEqual([row["sequence"] for row in rows], ["1", "3", "4"])
        self.assertEqual(list(rows[0]), list(capture.FIELDS))
        self.assertEqual(rows[0]["gyro_y_dps"], "-3.0")
        self.assertTrue(rows[0]["host_time_utc"].endswith("+00:00"))
        self.assertIn("missing=1", stderr)
        self.assertIn("malformed=1", stderr)

    def test_timeout_without_samples(self):
        self.start("--timeout", "0.2")
        os.write(self.master, b"firmware logs only\n")
        code, stderr, rows = self.finish()
        self.assertEqual(code, 1)
        self.assertEqual(rows, [])
        self.assertIn("No valid MPU sample", stderr)

    def test_disconnect_preserves_csv(self):
        self.start("--timeout", "2")
        os.write(self.master, record())
        # Wait for the periodic flush to prove receipt before unplugging.
        import time
        deadline = time.monotonic() + 3
        while "0.1" not in self.output.read_text():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        os.close(self.master)
        self.master = None
        code, stderr, rows = self.finish()
        self.assertEqual(code, 1)
        self.assertEqual(len(rows), 1)
        self.assertIn("Capture failed", stderr)

    def test_ctrl_c_flushes_csv(self):
        self.start("--timeout", "2")
        os.write(self.master, record())
        import time
        deadline = time.monotonic() + 3
        while "0.1" not in self.output.read_text():
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.02)
        self.process.send_signal(signal.SIGINT)
        code, stderr, rows = self.finish()
        self.assertEqual(code, 0, stderr)
        self.assertEqual(len(rows), 1)

    def test_existing_output_is_preserved(self):
        self.output.write_text("keep this recording\n")
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--port", self.port, "--output", str(self.output)],
            capture_output=True, timeout=5,
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(self.output.read_text(), "keep this recording\n")


if __name__ == "__main__":
    unittest.main()
