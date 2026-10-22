#!/usr/bin/env python3
"""Read Digipet's retained power log without resetting or clearing the device."""
import argparse
import csv
import io
import time
from pathlib import Path
import serial

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('port', help='USB serial port, e.g. /dev/ttyACM1')
parser.add_argument('output', type=Path, help='Destination CSV')
args = parser.parse_args()
# Keep pyserial's matching DTR/RTS defaults. Explicitly deasserting these
# on this board's USB/JTAG interface resets the chip and loses pending RTC data.
with serial.Serial(args.port, baudrate=115200, timeout=1) as device:
    device.reset_input_buffer()
    device.write(b'POWERLOG\n')
    deadline = time.monotonic() + 30
    rows = []
    started = finished = False
    while time.monotonic() < deadline:
        line = device.readline().decode('utf-8', errors='replace').strip()
        if line == 'POWERLOG BEGIN v1':
            started = True
        elif started and line == 'POWERLOG END':
            finished = True
            break
        elif started:
            rows.append(line)
    if not finished:
        raise SystemExit('No complete POWERLOG response. Check the port and wake the device if needed.')
    data = '\n'.join(rows) + '\n'
    parsed = list(csv.reader(io.StringIO(data)))
    if not parsed or any(len(row) != 12 for row in parsed):
        raise SystemExit('Malformed response; output was not written.')
    args.output.write_text(data)
    print(f'Saved {len(parsed) - 1} records to {args.output}')
