#!/usr/bin/env python3
"""Snapshots of the screens of a device, made by dgx_screen_snapshot_print() of DGX, read off its serial port or off a
log saved by any terminal, and written as .dgxi files and PNG pictures.

    dgx_snapshot.py PORT [--baud N] [--out DIR] [--reset] [--seconds N]   the port read, its log passed through
    dgx_snapshot.py --log FILE [--out DIR]                                 the snapshots of a saved log

A snapshot is the lines between "DGXSNAP BEGIN <name>" and "DGXSNAP END <name> <length> <crc32>": the bytes of a
.dgxi file in base64. Other lines in between, of the log of another task, are passed by; a snapshot whose length or
CRC-32 does not match is told and not written. Every snapshot goes to DIR/<name>.dgxi and DIR/<name>.png, a name
taken twice as <name>-2 and on. The port is opened with DTR and RTS low, which keeps a board out of reset; --reset
pulses RTS first, to read a log from the start. Reading a port needs pyserial, which ESP-IDF brings; Ctrl-C ends it.
"""
import argparse
import base64
import binascii
import os
import re
import sys
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dgx_images import DgxiError, to_png  # noqa: E402

BEGIN = re.compile(r"DGXSNAP BEGIN ([A-Za-z0-9._-]+)\s*$")
END = re.compile(r"DGXSNAP END ([A-Za-z0-9._-]+) (\d+) ([0-9a-f]{8})\s*$")
BASE64 = re.compile(r"[A-Za-z0-9+/]+={0,2}")


class Snapshots:
    """the lines of a log taken one by one: the snapshots in them written, the rest passed to `echo`"""

    def __init__(self, out, echo=None):
        self.out, self.echo = out, echo
        self.name, self.parts, self.taken, self.saved, self.spoiled = None, [], set(), [], []

    def line(self, text):
        text = text.rstrip("\r\n")
        begin = BEGIN.search(text)
        if begin:
            self.name, self.parts = begin.group(1), []
            return
        end = END.search(text)
        if end and self.name == end.group(1):
            self.finish(self.name, int(end.group(2)), int(end.group(3), 16))
            self.name = None
            return
        if self.name is not None and BASE64.fullmatch(text.strip()):
            self.parts.append(text.strip())
            return
        if self.echo:
            self.echo(text)

    def finish(self, name, length, crc):
        try:
            data = base64.b64decode("".join(self.parts), validate=True)
        except binascii.Error:
            data = b""
        if len(data) != length or zlib.crc32(data) & 0xFFFFFFFF != crc:
            self.spoiled.append(name)
            print("dgx_snapshot: %s spoiled on its way: %d bytes of %d, or its CRC does not match; not written" % (name, len(data), length),
                  file=sys.stderr)
            return
        base, k = name, 2
        while name in self.taken:
            name, k = "%s-%d" % (base, k), k + 1
        self.taken.add(name)
        os.makedirs(self.out, exist_ok=True)
        path = os.path.join(self.out, name)
        with open(path + ".dgxi", "wb") as f:
            f.write(data)
        try:
            with open(path + ".png", "wb") as f:
                f.write(to_png(data))
        except DgxiError as e:
            print("dgx_snapshot: %s: %s" % (name, e), file=sys.stderr)
        self.saved.append(path)
        print("dgx_snapshot: %s.dgxi and .png written" % path, file=sys.stderr)


def read_port(port, baud, seconds, reset, snapshots):
    try:
        import serial
    except ImportError:
        print("dgx_snapshot: reading a port needs pyserial: run it with the Python of ESP-IDF, or pip install pyserial", file=sys.stderr)
        return 2
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, baud, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    if reset:
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    end = time.time() + seconds if seconds else None
    pending = b""
    try:
        while end is None or time.time() < end:
            pending += s.read(4096)
            *lines, pending = pending.split(b"\n")
            for line in lines:
                snapshots.line(line.decode("utf-8", "replace"))
    except KeyboardInterrupt:
        pass
    finally:
        s.close()
    return 0


def main(argv):
    parser = argparse.ArgumentParser(description="Snapshots of the screens of a device, as .dgxi files and PNG pictures.")
    parser.add_argument("port", nargs="?", help="the serial port of the device")
    parser.add_argument("--log", help="a saved log to read instead of a port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default=".", help="where the files go, the current directory if not given")
    parser.add_argument("--reset", action="store_true", help="reset the board first, by RTS")
    parser.add_argument("--seconds", type=float, default=0, help="read the port so long; until Ctrl-C if not given")
    args = parser.parse_args(argv[1:])
    if bool(args.port) == bool(args.log):
        parser.error("give a port or --log FILE")
    snapshots = Snapshots(args.out, echo=None if args.log else print)
    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            for line in f:
                snapshots.line(line)
        code = 0
    else:
        code = read_port(args.port, args.baud, args.seconds, args.reset, snapshots)
    if snapshots.name is not None:
        print("dgx_snapshot: %s has no end: the log stops inside it" % snapshots.name, file=sys.stderr)
    return code or (1 if snapshots.spoiled else 0)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
