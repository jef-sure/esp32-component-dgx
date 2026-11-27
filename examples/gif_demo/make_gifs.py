#!/usr/bin/env python3
"""Writes the three animations of the demo, main/spinner.gif, main/ball.gif and main/bar.gif, drawn here pixel by pixel
and written as GIF by the standard library of Python alone.

Run it after changing an animation: python3 make_gifs.py

- spinner.gif: 48x48, 12 frames of 80 ms, for ever; twelve dots going round, the background transparent.
- ball.gif: 80x64, 16 frames of 50 ms, for ever; a ball bouncing on a floor, squashed where it lands, with its shadow.
- bar.gif: 120x20, 11 frames of 120 ms and the last of 600 ms, played 3 times; a bar filling from 0 to 100%.
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


# ---- GIF ----

def _blocks(data):
    """data cut into sub-blocks of 255 bytes at most, and the empty one that ends them"""
    return b"".join(bytes((len(data[i:i + 255]),)) + data[i:i + 255] for i in range(0, len(data), 255)) + b"\0"


def _lzw(indices, min_size=8):
    """the LZW codes of GIF for the indices, from a code size of min_size + 1 to 12 bits, the table cleared when full"""
    clear, end = 1 << min_size, (1 << min_size) + 1
    out, acc, nbits = bytearray(), 0, 0
    size, table, free = min_size + 1, {}, end + 1

    def put(code):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += size
        while nbits >= 8:
            out.append(acc & 255)
            acc >>= 8
            nbits -= 8

    put(clear)
    prefix = None
    for k in indices:
        if prefix is None:
            prefix = k
            continue
        code = table.get((prefix, k))
        if code is not None:
            prefix = code
            continue
        put(prefix)
        if free < 4096:
            table[(prefix, k)] = free
            free += 1
            # the decoder adds an entry a code later: it reads the next code wider once its own table reaches the width
            if free > (1 << size) and size < 12:
                size += 1
        else:
            put(clear)
            size, table, free = min_size + 1, {}, end + 1
        prefix = k
    if prefix is not None:
        put(prefix)
    put(end)
    if nbits:
        out.append(acc & 255)
    return bytes(out)


def write_gif(path, w, h, palette, frames, delays, loops, transparent=None, disposal=1):
    """frames of w x h indices into palette, each with its delay in hundredths of a second; loops 0 for ever"""
    table = bytes(c for e in palette for c in e) + bytes(3 * (256 - len(palette)))
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", w, h, 0xF7, 0, 0) + table)
    # the application block: its name, then a sub-block of 3 bytes, 1 and the times it is played
    out += b"\x21\xff\x0bNETSCAPE2.0\x03\x01" + struct.pack("<H", loops) + b"\0"
    for indices, delay in zip(frames, delays):
        packed = disposal << 2 | (1 if transparent is not None else 0)
        out += b"\x21\xf9\x04" + struct.pack("<BHB", packed, delay, transparent or 0) + b"\0"
        out += b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, 0) + b"\x08" + _blocks(_lzw(indices))
    out += b"\x3b"
    with open(path, "wb") as f:
        f.write(out)
    return len(out)


def _mix(a, b, t):
    return tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))


# ---- the spinner ----

def spinner():
    w = h = 48
    palette = [(0, 0, 0)] + [_mix((24, 52, 64), (120, 232, 255), i / 11.0) for i in range(12)]
    frames = []
    for f in range(12):
        px = [0] * (w * h)
        for i in range(12):
            a = math.radians(i * 30 - 90)
            cx, cy = 23.5 + 17 * math.cos(a), 23.5 + 17 * math.sin(a)
            level = 12 - (f - i) % 12  # the dot of the frame brightest, those behind it dimmer
            for y in range(h):
                for x in range(w):
                    if (x - cx) ** 2 + (y - cy) ** 2 <= 4.3 ** 2:
                        px[y * w + x] = level
        frames.append(px)
    return write_gif(os.path.join(HERE, "main", "spinner.gif"), w, h, palette, frames, [8] * 12, 0, transparent=0, disposal=2)


# ---- the ball ----

def ball():
    w, h, floor = 80, 64, 50
    sky = [_mix((34, 52, 104), (126, 166, 214), i / 15.0) for i in range(16)]
    ground, edge, shadow = (78, 62, 44), (112, 90, 64), (46, 36, 26)
    shades = [_mix((84, 16, 8), (255, 124, 70), i / 31.0) for i in range(32)]
    palette = [(0, 0, 0)] + sky + [ground, edge, shadow] + shades + [(255, 236, 214)]
    GROUND, EDGE, SHADOW, SHADE, SHINE = 17, 18, 19, 20, 52
    light = (-0.45, -0.6, 0.66)
    n = math.sqrt(sum(c * c for c in light))
    light = tuple(c / n for c in light)
    frames = []
    for f in range(16):
        t = f / 16.0
        up = 1 - (2 * t - 1) ** 2  # 0 on the floor, 1 at the top
        r = 10.0
        sx, sy = (1.25, 0.78) if f == 0 else (0.92, 1.1) if f in (1, 15) else (1.0, 1.0)
        cx, cy = 39.5, floor - r * sy - 34 * up
        px = []
        for y in range(h):
            row = 1 + min(15, y * 16 // floor) if y < floor else (EDGE if y == floor else GROUND)
            px += [row] * w
        # the shadow on the floor, smaller and further from the ball the higher it is
        sw, sh = 12 * (1 - 0.45 * up), 2.6 * (1 - 0.45 * up)
        for y in range(floor, h):
            for x in range(w):
                if ((x - 39.5) / sw) ** 2 + ((y - floor - 2.5) / sh) ** 2 <= 1:
                    px[y * w + x] = SHADOW
        for y in range(h):
            for x in range(w):
                nx, ny = (x - cx) / (r * sx), (y - cy) / (r * sy)
                d2 = nx * nx + ny * ny
                if d2 > 1:
                    continue
                nz = math.sqrt(1 - d2)
                lit = max(0.0, nx * light[0] + ny * light[1] + nz * light[2])
                px[y * w + x] = SHINE if lit > 0.975 else SHADE + int(round((0.18 + 0.82 * lit) * 31))
        frames.append(px)
    return write_gif(os.path.join(HERE, "main", "ball.gif"), w, h, palette, frames, [5] * 16, 0)


# ---- the bar ----

def bar():
    w, h, radius = 120, 20, 6
    palette = [(0, 0, 0), (24, 26, 32), (176, 180, 192), (52, 56, 66)] + [_mix((40, 150, 70), (150, 236, 110), i / 7.0) for i in range(8)]
    PAPER, BORDER, TRACK, FILL = 1, 2, 3, 4

    def inside(x, y, x0, y0, x1, y1, r):
        """whether the pixel x, y lies in the rectangle x0..x1, y0..y1 with corners of radius r"""
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        dx = max(x0 + r - x, 0, x - (x1 - r))
        dy = max(y0 + r - y, 0, y - (y1 - r))
        return dx * dx + dy * dy <= r * r

    frames = []
    for f in range(11):
        filled = 3 + (w - 6) * f // 10
        px = []
        for y in range(h):
            for x in range(w):
                if not inside(x, y, 0, 0, w - 1, h - 1, radius):
                    px.append(PAPER)
                elif not inside(x, y, 2, 2, w - 3, h - 3, radius - 2):
                    px.append(BORDER)
                elif x < filled and inside(x, y, 3, 3, w - 4, h - 4, radius - 3):
                    px.append(FILL + min(7, (x - 3) * 8 // (w - 6)))
                else:
                    px.append(TRACK)
        frames.append(px)
    return write_gif(os.path.join(HERE, "main", "bar.gif"), w, h, palette, frames, [12] * 10 + [60], 3)


if __name__ == "__main__":
    for make in (spinner, ball, bar):
        print("%s: %d bytes" % (make.__name__, make()))
