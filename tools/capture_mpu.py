#!/usr/bin/env python3
"""Record MPU1 console records from the ESP32 as a timestamped CSV file."""

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime, timezone
import math
from pathlib import Path
import sys
import time


FIELDS = (
    "host_time_utc", "sequence", "device_timestamp_us",
    "acc_x_g", "acc_y_g", "acc_z_g",
    "gyro_x_dps", "gyro_y_dps", "gyro_z_dps",
)
MAX_LINE_BYTES = 512


def parse_sample(line):
    """Return (sequence, timestamp, six axes), None for logs; reject bad records."""
    if not line.startswith(b"MPU1,"):
        return None
    parts = line.decode("ascii").strip().split(",")
    if len(parts) != 9:
        raise ValueError("expected sequence, timestamp and six axes")
    sequence, timestamp = int(parts[1]), int(parts[2])
    axes = tuple(float(value) for value in parts[3:])
    if not 0 <= sequence <= 0xFFFFFFFF or not 0 <= timestamp <= 0x7FFFFFFFFFFFFFFF:
        raise ValueError("sequence or timestamp out of range")
    if not all(math.isfinite(value) for value in axes):
        raise ValueError("non-finite sensor value")
    return (sequence, timestamp, *axes)


class LineBuffer:
    """Reassemble split reads, dropping oversized lines through their newline."""

    def __init__(self):
        self.pending = bytearray()
        self.discarding = False
        self.oversized = 0

    def feed(self, chunk):
        for byte in chunk:
            if byte == 10:
                if not self.discarding:
                    yield bytes(self.pending)
                self.pending.clear()
                self.discarding = False
            elif not self.discarding:
                self.pending.append(byte)
                if len(self.pending) > MAX_LINE_BYTES:
                    self.pending.clear()
                    self.discarding = True
                    self.oversized += 1


@dataclass
class Stats:
    samples: int = 0
    malformed: int = 0
    ignored: int = 0
    missing: int = 0
    restarts: int = 0
    duplicates: int = 0
    timing_gaps: int = 0
    previous: tuple | None = None

    def observe(self, sample):
        sequence, timestamp = sample[:2]
        notice = None
        if self.previous is not None:
            prev_sequence, prev_timestamp = self.previous
            step = (sequence - prev_sequence) & 0xFFFFFFFF
            if timestamp < prev_timestamp or step >= 0x80000000:
                self.restarts += 1
                notice = "Device restart or out-of-order sample detected."
            elif step == 0:
                self.duplicates += 1
            else:
                self.missing += step - 1
                if not 5000 <= timestamp - prev_timestamp <= 15000:
                    self.timing_gaps += 1
        self.previous = (sequence, timestamp)
        self.samples += 1
        return notice

    def summary(self, elapsed, oversized):
        return (
            f"{self.samples} samples, {self.samples / max(elapsed, 0.001):.1f} rows/s; "
            f"missing={self.missing}, malformed={self.malformed}, "
            f"restarts={self.restarts}, duplicates={self.duplicates}, "
            f"timing_gaps={self.timing_gaps}, ignored_logs={self.ignored}, "
            f"oversized_lines={oversized}"
        )


def positive_float(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return number


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list-ports", action="store_true", help="list serial devices and exit")
    parser.add_argument("--port", help="serial device, e.g. /dev/ttyUSB0 or /dev/ttyACM0")
    parser.add_argument("--baud", type=positive_int, default=460800,
                        help="firmware UART console baud rate (default: 460800)")
    parser.add_argument("--duration", type=positive_float, metavar="SECONDS",
                        help="stop after this many seconds from opening the port")
    parser.add_argument("--timeout", type=positive_float, default=30.0, metavar="SECONDS",
                        help="fail after this long without a valid sample (default: 30)")
    parser.add_argument("--output", type=Path,
                        help="new CSV path (default: captures/mpu_<UTC timestamp>.csv)")
    args = parser.parse_args(argv)
    if not args.list_ports and not args.port:
        parser.error("--port is required unless --list-ports is used")
    return args


def capture(args, serial_module):
    output = args.output or Path("captures") / (
        "mpu_" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ") + ".csv"
    )
    stats = Stats()
    lines = LineBuffer()
    started = None
    exit_code = 0
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation prevents accidentally replacing an earlier recording.
        with output.open("x", newline="", encoding="utf-8") as csv_file:
            writer = csv.writer(csv_file)
            writer.writerow(FIELDS)
            csv_file.flush()
            # Set control lines before opening to reduce unintended board resets.
            # Some USB adapters/drivers can still briefly toggle them on open.
            with serial_module.Serial(port=None, baudrate=args.baud, timeout=0.1,
                                      exclusive=True) as port:
                port.dtr = False
                port.rts = False
                port.port = args.port
                port.open()
                started = time.monotonic()
                last_sample = last_flush = last_report = started
                print(f"Recording {args.port} at {args.baud} baud to {output}", file=sys.stderr)
                print("Press Ctrl+C to stop. Waiting for MPU1 records...", file=sys.stderr)
                try:
                    while True:
                        now = time.monotonic()
                        if args.duration is not None and now - started >= args.duration:
                            break
                        if now - last_sample >= args.timeout:
                            print(
                                f"No valid MPU sample for {args.timeout:g} s. Check the port, "
                                "baud rate, sensor, and CONFIG_MPU_SERIAL_STREAM firmware setting.",
                                file=sys.stderr,
                            )
                            exit_code = 1
                            break
                        chunk = port.read(min(1024, port.in_waiting or 1))
                        for line in lines.feed(chunk):
                            try:
                                sample = parse_sample(line)
                            except (ValueError, UnicodeError):
                                stats.malformed += 1
                                continue
                            if sample is None:
                                stats.ignored += 1
                                continue
                            received = datetime.now(timezone.utc).isoformat(timespec="microseconds")
                            writer.writerow((received, *sample))
                            notice = stats.observe(sample)
                            if notice:
                                print(notice, file=sys.stderr)
                            last_sample = time.monotonic()
                        now = time.monotonic()
                        if now - last_flush >= 1.0:
                            csv_file.flush()
                            last_flush = now
                        if now - last_report >= 5.0:
                            print(stats.summary(now - started, lines.oversized), file=sys.stderr)
                            last_report = now
                finally:
                    csv_file.flush()
    except KeyboardInterrupt:
        print("\nCapture stopped.", file=sys.stderr)
    except (OSError, serial_module.SerialException) as exc:
        print(f"Capture failed: {exc}", file=sys.stderr)
        exit_code = 1
    if started is not None:
        print(stats.summary(time.monotonic() - started, lines.oversized), file=sys.stderr)
        print(f"CSV: {output.resolve()}", file=sys.stderr)
        if lines.pending or lines.discarding:
            print("Discarded an incomplete final serial line.", file=sys.stderr)
        if stats.samples == 0:
            print("No samples captured.", file=sys.stderr)
            exit_code = 1
    return exit_code


def main(argv=None):
    args = arguments(argv)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        print("Install the dependency: python3 -m pip install -r tools/requirements.txt", file=sys.stderr)
        return 1
    if args.list_ports:
        ports = sorted(list_ports.comports(), key=lambda item: item.device)
        for port in ports:
            print(f"{port.device}\t{port.description}\t{port.hwid}")
        if not ports:
            print("No serial ports found. Connect the ESP32's UART USB port.")
        return 0
    return capture(args, serial)


if __name__ == "__main__":
    sys.exit(main())
