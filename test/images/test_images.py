#!/usr/bin/env python3
"""
Pictures of PNG files as .dgxi files, tools/dgx_images.py, against the reader and the drawing of DGX: every file made
here is read and drawn by dgxi_dump (a program of DGX built for the host under AddressSanitizer), at an address
divisible by 4 and at the three after it, and what DGX draws is compared with the PNG pixel by pixel.

- PNG: every picture of PngSuite (pngsuite/, the test pictures of PNG by Willem van Schaik) reads into the colors PIL
  gives, where PIL is installed, and every spoiled one is refused.
- Every depth and kind: the values a virtual screen of the depth holds, the plane of opacity, the palette.
- The colors: exact palettes, full and not, a refusal with the number of colors, REDUCE, DITHER, 1 bit with and
  without transparency, INK, ALPHA, BACKGROUND, ORIGIN, sets.
- Needs: files of needs, their errors with the file and the line, the files build makes, dgx_assets.S assembled and
  linked on the host with every file at an address divisible by 4.

Run: make -C test/images.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DGX = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(DGX, "tools"))
import dgx_images as d  # noqa: E402

DUMP = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "build", "dgxi_dump")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print("FAIL: %s" % what)


def refused(call, words):
    """the call refused with a DgxiError whose text has every one of words"""
    try:
        call()
    except d.DgxiError as e:
        missing = [w for w in words if w not in str(e)]
        check(not missing, "the refusal '%s' says %s" % (e, missing))
        return str(e)
    check(False, "refused: %s" % words)
    return ""


# ---- PNG pictures made here ----

def png(w, h, pixels, color=6, depth=8, palette=None, trns=None, texts=()):
    """a PNG of pixels: (r, g, b, a) for color 6, (r, g, b) for 2, gray for 0, (g, a) for 4, an index for 3"""
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row = pixels[y * w:(y + 1) * w]
        samples = []
        for p in row:
            samples += list(p) if isinstance(p, tuple) else [p]
        if depth == 16:
            for s in samples:
                raw += struct.pack(">H", s)
        elif depth == 8:
            raw += bytes(samples)
        else:
            bits = 0
            n = 0
            for s in samples:
                bits = bits << depth | s
                n += depth
                if n == 8:
                    raw.append(bits)
                    bits = n = 0
            if n:
                raw.append(bits << (8 - n))
    del channels

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, 0))
    if palette:
        out += chunk(b"PLTE", bytes(c for e in palette for c in e))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    for key, value in texts:
        out += chunk(b"tEXt", key.encode("latin-1") + b"\0" + value.encode("latin-1"))
    return out + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


def gradient(w, h, holes=False):
    """RGBA pixels: a gradient of every channel, transparent and half-transparent places where holes"""
    out = []
    for y in range(h):
        for x in range(w):
            r, g, b = x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), (x * 37 + y * 91) % 256
            a = 255
            if holes and (x + y) % 5 == 0:
                a = 0
            elif holes and (x + y) % 7 == 0:
                a = 100
            out.append((r, g, b, a))
    return out


# ---- what DGX reads and draws ----

def dump(data, shift=0):
    """the file of data read and drawn by DGX: a dict, or the code of a refusal"""
    with tempfile.NamedTemporaryFile(suffix=".dgxi", delete=False) as f:
        f.write(data)
        name = f.name
    try:
        run = subprocess.run([DUMP, name, str(shift)], capture_output=True, text=True)
    finally:
        os.unlink(name)
    if "ERROR: AddressSanitizer" in run.stderr or "runtime error" in run.stderr:
        check(False, "dgxi_dump under the sanitizers: %s" % run.stderr[-2000:])
    out = {"pictures": [], "palette": []}
    for line in run.stdout.splitlines():
        words = line.split()
        if words[0] == "refused":
            return int(words[1])
        nums = [int(w) for w in words[1:]]
        if words[0] == "file":
            out.update(bits=nums[0], kind=nums[1], count=nums[2], colors=nums[3], first=nums[4])
        elif words[0] == "entry":
            out["palette"].append(tuple(nums))
        elif words[0] == "animation":
            out["animation"] = nums
        elif words[0] == "picture":
            out["pictures"].append({"w": nums[0], "h": nums[1], "origin": (nums[2], nums[3]), "rows": [], "alpha": []})
        elif words[0] in ("row", "alpha"):
            out["pictures"][-1]["rows" if words[0] == "row" else "alpha"].append(nums)
    check(run.returncode == 0, "dgxi_dump exits with %d: %s" % (run.returncode, run.stderr[-500:]))
    return out


def dumped(data):
    """read at an address divisible by 4 and at the three after it, the same every time"""
    first = dump(data, 0)
    for shift in (1, 2, 3):
        check(dump(data, shift) == first, "the file read at an address 4k + %d is read the same" % shift)
    return first


def values(pic):
    return [v for row in pic["rows"] for v in row]


# ---- expected values, counted here without the tool ----

def lvl(v, top):
    return int(v * top / 255.0 + 0.5)


def gray(r, g, b):
    return (r * 77 + g * 150 + b * 29) >> 8


def screen_value(bits, r, g, b, a):
    """what dgx_get_pixel() gives for the pixel of a virtual screen of the depth made of this color: the high bits of
    each channel, as DGX_RGB_12(), DGX_RGB_16(), DGX_RGB_18() and dgx_screen_rgb() take them, so that a color of a
    picture is the color the program writes"""
    if bits == 1:
        return 1 if gray(r, g, b) >= 128 else 0
    if bits == 4:
        return gray(r, g, b) & 0xF0
    if bits == 8:
        return gray(r, g, b)
    if bits == 12:
        return (r & 0xF0) << 8 | (g & 0xF0) << 4 | (b & 0xF0)
    if bits == 16:
        return (r & 0xF8) << 8 | (g & 0xFC) << 3 | (b & 0xF8) >> 3
    if bits == 18:
        return (r & 0xFC) << 16 | (g & 0xFC) << 8 | (b & 0xFC)
    if bits == 24:
        return r << 16 | g << 8 | b
    return a << 24 | r << 16 | g << 8 | b


# ---- PngSuite ----

def test_pngsuite():
    suite = os.path.join(HERE, "pngsuite")
    try:
        from PIL import Image
    except ImportError:
        Image = None
        print("note: PIL is not installed, PngSuite is read but not compared")
    files = sorted(f for f in os.listdir(suite) if f.endswith(".png"))
    check(len(files) == 176, "PngSuite has its 176 pictures, not %d" % len(files))
    for name in files:
        data = open(os.path.join(suite, name), "rb").read()
        if name.startswith("x"):
            refused(lambda: d.read_png(data), [])
            continue
        try:
            w, h, rgba, _ = d.read_png(data)
        except d.DgxiError as e:
            check(False, "%s is read: %s" % (name, e))
            continue
        if Image is None:
            continue
        im = Image.open(os.path.join(suite, name))
        im.load()
        check(im.size == (w, h), "%s: its size" % name)
        if im.mode in ("L", "I;16", "I", "1"):
            # PIL keeps the transparent gray of tRNS at the depth of the file and its 16-bit gray unscaled: counted here as PNG says
            depth = data[24]
            raw = list(im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata())
            if im.mode == "L" and depth < 8:
                raw = [v * ((1 << depth) - 1) // 255 for v in raw]
            elif im.mode == "1":
                raw = [1 if v else 0 for v in raw]
            t = im.info.get("transparency")
            ref = bytearray()
            for v in raw:
                g = v if depth == 8 else (v * 255 + 32767) // 65535 if depth == 16 else v * 255 // ((1 << depth) - 1)
                ref += bytes((g, g, g, 0 if t is not None and v == t else 255))
            check(bytes(rgba) == bytes(ref), "%s: the pixels of a gray picture as PNG says" % name)
            continue
        ref = im.convert("RGBA").tobytes()
        # 16 bits are rounded to the nearest 8 here, where PIL takes the high byte: off by one at most
        most = 1 if data[24] == 16 else 0
        worst = max(abs(a - b) for a, b in zip(rgba, ref))
        check(worst <= most, "%s: the pixels as PIL reads them, off by %d" % (name, worst))


# ---- every depth and kind ----

def test_depths():
    w, h = 13, 7
    pixels = gradient(w, h, holes=True)
    data = png(w, h, pixels)
    for bits in d.DEPTHS:
        kinds = ["pixels"] + (["indexed"] if bits <= 8 else [])
        for kind in kinds:
            args = {"bits": bits, "kind": kind}
            if kind == "indexed":
                args["reduce"] = True
            out = dumped(d.make([data], **args))
            what = "%d bits %s" % (bits, kind)
            if not isinstance(out, dict):
                check(False, "%s: refused by DGX with %s" % (what, out))
                continue
            check(out["bits"] == bits and out["count"] == 1, "%s: the head" % what)
            pic = out["pictures"][0]
            check((pic["w"], pic["h"]) == (w, h), "%s: the size" % what)
            got = values(pic)
            if kind == "pixels":
                check(out["kind"] == d.PIXELS and out["colors"] == 0, "%s: a file of pixels" % what)
                want = [screen_value(bits, *p) for p in pixels]
                check(got == want, "%s: the values of the pixels" % what)
                if bits == 32:
                    check(not pic["alpha"], "%s: no plane, the opacity is in the pixels" % what)
                else:
                    check([v for row in pic["alpha"] for v in row] == [p[3] for p in pixels], "%s: the plane of opacity" % what)
            else:
                check(out["kind"] == d.INDEXED and out["first"] == 1, "%s: indexed with transparency" % what)
                for v, p in zip(got, pixels):
                    if p[3] < 128:
                        check(v == 0, "%s: a transparent pixel is 0" % what)
                        break
                check(all(v <= out["colors"] for v in got), "%s: every value has its entry" % what)
    # an odd width at every depth: rows of indexed pictures from a new byte, pixels one after another
    for w in (1, 3, 9, 17):
        pixels = gradient(w, 3)
        data = png(w, 3, pixels)
        for bits in (1, 4, 12):
            out = dumped(d.make([data], bits=bits, kind="pixels"))
            check(values(out["pictures"][0]) == [screen_value(bits, *p) for p in pixels], "width %d, %d bits of pixels" % (w, bits))
        two = [(0, 0, 0, 255) if (x + y) % 3 else (255, 255, 255, 255) for y in range(3) for x in range(w)]
        out = dumped(d.make([png(w, 3, two)], bits=1))
        want = [1 if p[0] else 0 for p in two]
        if len(set(want)) == 1:
            want = [1] * len(want)
        check(values(out["pictures"][0]) == want, "width %d, 1 bit indexed" % w)


# ---- the colors ----

def opaque(n, w=None):
    """n different opaque colors, one a pixel"""
    cols = [((i * 53) % 256, (i * 97) % 256, (i * 11 + i // 7) % 256, 255) for i in range(n)]
    return png(n, 1, cols), cols


def test_colors():
    # a palette of fewer colors than the depth holds: 0 is transparent, every color exact
    data, cols = opaque(3)
    out = dumped(d.make([data], bits=4))
    check(out["colors"] == 3 and out["first"] == 1, "3 colors at 4 bits: a palette of 3, from the value 1")
    check([out["palette"][v - 1][:3] for v in values(out["pictures"][0])] == [c[:3] for c in cols], "3 colors at 4 bits: every pixel its color")
    # all the colors of the depth: a full palette, from the value 0
    data, cols = opaque(16)
    out = dumped(d.make([data], bits=4))
    check(out["colors"] == 16 and out["first"] == 0, "16 colors at 4 bits: a full palette, from the value 0")
    check([out["palette"][v][:3] for v in values(out["pictures"][0])] == [c[:3] for c in cols], "16 colors at 4 bits: every pixel its color")
    data = png(2, 1, [(0, 0, 0, 255), (255, 255, 255, 255)])
    out = dumped(d.make([data], bits=1))
    check(out["colors"] == 2 and out["first"] == 0, "2 opaque colors at 1 bit: a full palette")
    data, cols = opaque(256)
    out = dumped(d.make([png(16, 16, cols)], bits=8))
    check(out["colors"] == 256 and out["first"] == 0, "256 colors at 8 bits: a full palette")
    # more colors than the depth holds
    refused(lambda: d.make([opaque(17)[0]], bits=4), ["17 colors", "4 bits hold 16", "REDUCE"])
    out = dumped(d.make([opaque(17)[0]], bits=4, reduce=True))
    check(out["colors"] == 16 and out["first"] == 0, "17 colors reduced to 16 at 4 bits")
    with_hole = list(opaque(16)[1]) + [(0, 0, 0, 0)]
    refused(lambda: d.make([png(17, 1, with_hole)], bits=4), ["16 colors", "4 bits hold 15 with transparency"])
    out = dumped(d.make([png(17, 1, with_hole)], bits=4, reduce=True))
    check(out["colors"] == 15 and out["first"] == 1 and values(out["pictures"][0])[16] == 0, "16 colors and a hole reduced to 15")
    # REDUCE of a photo of many colors: every value has its entry, and the mean error is small
    w, h = 40, 30
    photo = gradient(w, h)
    for dither in (False, True):
        out = dumped(d.make([png(w, h, photo)], bits=4, reduce=True, dither=dither))
        got = values(out["pictures"][0])
        check(out["colors"] == 16 and all(0 <= v < 16 for v in got), "a photo reduced to 16 colors, dither %s" % dither)
        err = sum(abs(out["palette"][v][k] - p[k]) for v, p in zip(got, photo) for k in range(3)) / (3.0 * w * h)
        check(err < 40, "a photo reduced to 16 colors keeps near its colors, dither %s: %.1f" % (dither, err))
    # dither keeps the mean brightness of a ramp at 1 bit
    ramp = [(x * 255 // 63,) * 3 + (255,) for y in range(8) for x in range(64)]
    out = dumped(d.make([png(64, 8, ramp)], bits=1, kind="pixels", dither=True))
    ones = sum(values(out["pictures"][0]))
    want = sum(p[0] for p in ramp) / 255.0
    check(abs(ones - want) < 8, "a dithered ramp at 1 bit keeps its brightness: %d ones for %.1f" % (ones, want))
    plain = dumped(d.make([png(64, 8, ramp)], bits=1, kind="pixels"))
    check(values(plain["pictures"][0]) == [1 if p[0] >= 128 else 0 for p in ramp], "a ramp at 1 bit undithered: light from the middle gray")
    # 1 bit: with transparency the opaque pixels are 1 of their mean color
    icon = [(200, 0, 0, 255) if x % 2 else (0, 0, 0, 0) for x in range(6)] + [(100, 0, 0, 255)] * 6
    out = dumped(d.make([png(6, 2, icon)], bits=1))
    check(out["colors"] == 1 and out["first"] == 1, "1 bit with transparency: one entry, the ink")
    check(values(out["pictures"][0]) == [x % 2 for x in range(6)] + [1] * 6, "1 bit with transparency: the opaque pixels are 1")
    check(out["palette"][0] == (133, 0, 0, 255), "1 bit with transparency: the ink is the mean color, not %s" % (out["palette"][0],))
    # without: the light pixels are 1, or the dark ones with INK DARK
    bw = [(250, 250, 250, 255), (5, 5, 5, 255), (250, 250, 250, 255)]
    out = dumped(d.make([png(3, 1, bw)], bits=1))
    check(values(out["pictures"][0]) == [1, 0, 1] and out["palette"] == [(5, 5, 5, 255), (250, 250, 250, 255)], "1 bit: light is 1")
    out = dumped(d.make([png(3, 1, bw)], bits=1, ink="dark"))
    check(values(out["pictures"][0]) == [0, 1, 0] and out["palette"] == [(250, 250, 250, 255), (5, 5, 5, 255)], "1 bit, INK DARK: dark is 1")
    out = dumped(d.make([png(3, 1, [(5, 5, 5, 255)] * 3)], bits=1))
    check(values(out["pictures"][0]) == [1, 1, 1] and out["colors"] == 1, "1 bit of one color: one entry, every pixel 1")
    # ALPHA keeps the half-transparent pixels in the palette; without it they are opaque or holes
    soft = [(10, 20, 30, 255), (10, 20, 30, 128), (10, 20, 30, 40), (10, 20, 30, 0)]
    out = dumped(d.make([png(4, 1, soft)], bits=8))
    check(values(out["pictures"][0])[2:] == [0, 0] and set(out["palette"]) == {(10, 20, 30, 255)}, "no ALPHA: half opaque or a hole")
    out = dumped(d.make([png(4, 1, soft)], bits=8, alpha=True))
    got = values(out["pictures"][0])
    check(got[3] == 0 and [out["palette"][v - 1][3] for v in got[:3]] == [255, 128, 40], "ALPHA: the opacity in the entries")
    # BACKGROUND paints the transparent places over: no plane, no transparency
    out = dumped(d.make([png(4, 1, soft)], bits=16, background="ff0000"))
    check(not out["pictures"][0]["alpha"], "BACKGROUND: no plane")
    check(values(out["pictures"][0])[3] == screen_value(16, 255, 0, 0, 255), "BACKGROUND: a hole is the color")
    mixed = ((10 * 128 + 255 * 127 + 127) // 255, (20 * 128 + 127) // 255, (30 * 128 + 127) // 255)
    check(values(out["pictures"][0])[1] == screen_value(16, *mixed, 255), "BACKGROUND: half opaque mixed with the color")
    out = dumped(d.make([png(4, 1, soft)], bits=4, background=(0, 0, 0)))
    check(out["first"] == 1 and 0 not in values(out["pictures"][0]), "BACKGROUND of an indexed picture: no hole")
    # a plane only where the PNG has a pixel not quite opaque
    out = dumped(d.make([png(3, 1, [(1, 2, 3, 255)] * 3)], bits=16))
    check(not out["pictures"][0]["alpha"], "an opaque PNG has no plane")
    # a set: one palette, pictures of their own sizes, one origin
    a = png(2, 2, [(255, 0, 0, 255)] * 4)
    b = png(3, 1, [(0, 255, 0, 255), (255, 0, 0, 255), (0, 0, 0, 0)])
    out = dumped(d.make([a, b], bits=4, origin=(-3, 7)))
    check(out["count"] == 2 and out["colors"] == 2, "a set of two pictures: one palette of their two colors")
    check([(p["w"], p["h"], p["origin"]) for p in out["pictures"]] == [(2, 2, (-3, 7)), (3, 1, (-3, 7))], "a set: the sizes and the origin")
    red = out["palette"].index((255, 0, 0, 255)) + 1
    check(values(out["pictures"][0]) == [red] * 4 and values(out["pictures"][1])[1:] == [red, 0], "a set: the same color, the same value")
    # the text about the pictures
    t = png(1, 1, [(0, 0, 0, 255)], texts=[("Author", "Somebody"), ("Copyright", "CC0"), ("Software", "left out")])
    data = d.make([t], name="logo", sources=["logo.png"])
    about = data[struct.unpack("<I", data[24:28])[0]:][:struct.unpack("<I", data[28:32])[0]].decode()
    check("name: logo" in about and "source: logo.png" in about and "Author: Somebody" in about and "Copyright: CC0" in about
          and "Software" not in about, "the text about the pictures: %r" % about)
    # the same pictures make the same bytes
    check(d.make([a, b], bits=4) == d.make([a, b], bits=4), "the same pictures, the same bytes")


def test_mask():
    # a mask of 8 bits: how much each pixel is covered, the opacity of the PNG, exactly; drawn soft by DGX in 255 over 0
    alphas = [0, 17, 128, 200, 255, 1]
    row = [(255, 0, 0, a) for a in alphas]
    data = d.make([png(6, 1, row)], bits=8, kind="mask")
    check(struct.unpack("<H", data[4:6])[0] == 2 and data[7] == d.MASK, "a mask is of format 2, kind 3")
    out = dumped(data)
    check(out["kind"] == d.MASK and out["colors"] == 1 and values(out["pictures"][0]) == alphas, "a mask of 8 bits: %s" % values(out["pictures"][0]))
    check(out["palette"] == [(255, 0, 0, 255)], "a mask: its one entry the mean color it covers, %s" % out["palette"])
    out = dumped(d.make([png(6, 1, row)], bits=4, kind="mask"))
    check(values(out["pictures"][0]) == [lvl(a, 15) * 17 for a in alphas], "a mask of 4 bits: the nearest of 16 levels")
    # a PNG without transparency: the brightness, the light the ink, or the dark with INK DARK
    grays = [(g, g, g, 255) for g in (0, 60, 128, 255)]
    out = dumped(d.make([png(4, 1, grays)], bits=8, kind="mask"))
    check(values(out["pictures"][0]) == [0, 60, 128, 255], "a mask of an opaque PNG: its brightness")
    out = dumped(d.make([png(4, 1, grays)], bits=8, kind="mask", ink="dark"))
    check(values(out["pictures"][0]) == [255, 195, 127, 0], "a mask of an opaque PNG, INK DARK: the dark is the ink")
    # dithered: every value one of the levels, the mean kept
    ramp = [(255, 255, 255, x * 255 // 63) for x in range(64)]
    out = dumped(d.make([png(64, 1, ramp)], bits=4, kind="mask", dither=True))
    got = values(out["pictures"][0])
    check(all(v % 17 == 0 for v in got) and abs(sum(got) - sum(p[3] for p in ramp)) < 64 * 9, "a dithered mask keeps its mean")
    for args, words in (({"bits": 1, "kind": "mask"}, ["MASK of 1 bits"]), ({"bits": 16, "kind": "mask"}, ["MASK of 16 bits"]),
                        ({"bits": 8, "kind": "mask", "alpha": True}, ["ALPHA of a mask"]),
                        ({"bits": 8, "kind": "mask", "background": "000000"}, ["BACKGROUND of a mask"])):
        refused(lambda: d.make([png(1, 1, [(0, 0, 0, 255)])], **args), words)


# ---- GIF ----

def gif(w, h, frames, palette, loops=None, delays=None, disposals=None, transparent=None, interlace=False):
    """a GIF of frames of indices, each (left, top, width, height, indices), its LZW codes all of 9 bits: a clear
    code before every 254 of them keeps the table from growing"""
    def blocks(b):
        return b"".join(bytes((len(b[i:i + 255]),)) + b[i:i + 255] for i in range(0, len(b), 255)) + b"\0"

    def lzw(indices):
        codes, out, acc, nbits = [], bytearray(), 0, 0
        for k, i in enumerate(indices):
            if k % 254 == 0:
                codes.append(256)
            codes.append(i)
        codes.append(257)
        for c in codes:
            acc |= c << nbits
            nbits += 9
            while nbits >= 8:
                out.append(acc & 255)
                acc >>= 8
                nbits -= 8
        if nbits:
            out.append(acc & 255)
        return bytes(out)

    table = bytes(c for e in palette for c in e) + bytes(3 * (256 - len(palette)))
    out = b"GIF89a" + struct.pack("<HHBBB", w, h, 0xF7, 0, 0) + table
    if loops is not None:
        out += b"\x21\xff" + blocks(b"NETSCAPE2.0" + struct.pack("<BH", 1, loops))
    for k, (left, top, fw, fh, indices) in enumerate(frames):
        disposal = disposals[k] if disposals else 0
        delay = delays[k] if delays else 0
        packed = disposal << 2 | (1 if transparent is not None else 0)
        out += b"\x21\xf9\x04" + struct.pack("<BHB", packed, delay, transparent or 0) + b"\0"
        rows = list(range(fh))
        if interlace:
            rows = [y for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)) for y in range(start, fh, step)]
        stored = [indices[y * fw + x] for y in rows for x in range(fw)]
        out += b"\x2c" + struct.pack("<HHHHB", left, top, fw, fh, 0x40 if interlace else 0) + b"\x08" + blocks(lzw(stored))
    return out + b"\x3b"


def test_gif():
    pal = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    w, h = 9, 7
    f0 = (0, 0, w, h, [1] * (w * h))
    f1 = (2, 1, 4, 3, [2] * 12)
    f2 = (5, 3, 3, 3, [3, 0, 3, 0, 3, 0, 3, 0, 3])
    for interlace in (False, True):
        data = gif(w, h, [f0, f1, f2], pal, loops=2, delays=[5, 0, 30], disposals=[1, 2, 1], transparent=0, interlace=interlace)
        gw, gh, frames, delays, loops = d.read_gif(data)
        check((gw, gh, len(frames), delays, loops) == (w, h, 3, [50, 100, 300], 2), "a GIF: size, frames, delays, loops, interlace %s" % interlace)
        px = lambda fr, x, y: tuple(frames[fr][(y * w + x) * 4:(y * w + x) * 4 + 4])
        check(px(0, 4, 4) == (255, 0, 0, 255) and px(1, 3, 2) == (0, 255, 0, 255) and px(1, 0, 0) == (255, 0, 0, 255), "a GIF: a frame over the one before")
        # disposal 2 of frame 1 clears its box to transparent; transparent indices of frame 2 leave what is under
        check(px(2, 3, 2) == (0, 0, 0, 0) and px(2, 5, 3) == (0, 0, 255, 255) and px(2, 6, 3) == (255, 0, 0, 255), "a GIF: disposal and transparency")
    # made into a file: the delays and the loops, as DGX reads them, and every frame a picture
    data = gif(w, h, [f0, f1, f2], pal, loops=0, delays=[5, 7, 30], disposals=[1, 2, 1], transparent=0)
    out = dumped(d.make(gif=data, bits=16))
    check(out["count"] == 3 and out.get("animation") == [0, 50, 70, 300], "an animation of a GIF: %s" % out.get("animation"))
    check(values(out["pictures"][1])[1 * w + 3] == screen_value(16, 0, 255, 0, 255), "an animation of a GIF: the pixels of a frame")
    out = dumped(d.make([png(1, 1, [(1, 2, 3, 255)])] * 3, bits=16, delay=250, loops=4))
    check(out.get("animation") == [4, 250, 250, 250], "an animation of PNG pictures by DELAY and LOOPS: %s" % out.get("animation"))
    check(dumped(d.make([png(1, 1, [(1, 2, 3, 255)])], bits=16)).get("animation") is None, "a set without DELAY is no animation")
    refused(lambda: d.make([png(1, 1, [(0, 0, 0, 255)])], gif=data), ["PNG and GIF"])
    refused(lambda: d.make(gif=data, delay=100), ["DELAY of a GIF"])
    refused(lambda: d.make([png(1, 1, [(0, 0, 0, 255)])], loops=2), ["LOOPS without DELAY"])
    refused(lambda: d.make(gif=b"GIF89a" + bytes(7)), ["no pixels"])
    refused(lambda: d.make(gif=data[:40]), ["cut"])
    try:
        from PIL import Image
    except ImportError:
        print("note: PIL is not installed, GIF frames are not compared with it")
        return
    import io
    frames = []
    for k in range(6):
        im = Image.new("RGBA", (31, 19), (0, 0, 0, 0))
        for y in range(19):
            for x in range(31):
                if (x - 4 * k) ** 2 + (y - 9) ** 2 < 30:
                    im.putpixel((x, y), (200, 40 * k, 60, 255))
                elif x > 25 - k and y < 6:
                    im.putpixel((x, y), (30, 160, 220, 255))
        frames.append(im)
    for disposal in (0, 1, 2):
        buf = io.BytesIO()
        frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:], duration=60, loop=0, disposal=disposal, transparency=0)
        _, _, mine, _, _ = d.read_gif(buf.getvalue())
        ref = Image.open(io.BytesIO(buf.getvalue()))
        bad = 0
        for i in range(ref.n_frames):
            ref.seek(i)
            r = ref.convert("RGBA").tobytes()
            bad += sum(1 for o in range(0, len(r), 4) if (r[o + 3] > 0) != (mine[i][o + 3] > 0) or (r[o + 3] and r[o:o + 3] != mine[i][o:o + 3]))
        check(bad == 0, "the frames of a GIF of PIL, disposal %d, as PIL reads them: %d pixels differ" % (disposal, bad))


def test_back_to_png():
    # every kind read back as DGX draws it: through a PNG written and read again, the same pixels
    pictures = gradient(11, 5, holes=True)
    for args in ({"bits": 16}, {"bits": 12}, {"bits": 18}, {"bits": 24}, {"bits": 32}, {"bits": 1, "kind": "pixels"}, {"bits": 4, "kind": "pixels"},
                 {"bits": 8, "kind": "pixels"}, {"bits": 4, "reduce": True}, {"bits": 8, "kind": "mask"}, {"bits": 1}):
        data = d.make([png(11, 5, pictures)], **args)
        back = d.read_dgxi(data)
        w, h, _, _, rgba = back["pictures"][0]
        again = d.read_png(d.to_png(data))
        check(again[:3] == (w, h, rgba), "a picture of %s back to a PNG and read again" % args)
        if args.get("kind") is None and args["bits"] >= 12:
            # the colors of the file, of the depth, against what DGX holds: the dump
            out = dumped(data)
            got = values(out["pictures"][0])
            want = [screen_value(args["bits"], *rgba[i * 4:i * 4 + 4]) for i in range(w * h)]
            check(got == want, "a picture of %d bits read back in its own colors" % args["bits"])


def test_snapshots():
    # snapshots printed by DGX on a console, among lines of a log, read back by tools/dgx_snapshot.py
    run = subprocess.run([DUMP, "--snapshot"], capture_output=True, text=True)
    check(run.returncode == 0 and "DGXSNAP BEGIN sixteen" in run.stdout, "dgxi_dump --snapshot prints snapshots: %s" % run.stderr[-300:])
    lines = run.stdout.split("\n")
    # a line of another task inside a snapshot is passed by
    k = lines.index("DGXSNAP BEGIN sixteen") + 2
    lines.insert(k, "W (15) other: a line of another task")
    work = tempfile.mkdtemp(prefix="dgxsnap-")
    tool = os.path.join(DGX, "tools", "dgx_snapshot.py")
    try:
        log = os.path.join(work, "log.txt")
        open(log, "w").write("\n".join(lines))
        r = subprocess.run([sys.executable, "-I", tool, "--log", log, "--out", work], capture_output=True, text=True)
        check(r.returncode == 0, "dgx_snapshot --log: %s" % r.stderr)
        for name in ("sixteen", "four"):
            check(os.path.isfile(os.path.join(work, name + ".dgxi")) and os.path.isfile(os.path.join(work, name + ".png")), "%s.dgxi and .png written" % name)
        out = dumped(open(os.path.join(work, "sixteen.dgxi"), "rb").read())
        check(out["bits"] == 16 and values(out["pictures"][0]) == [(x * 0x0841 + y * 0x1000) & 0xFFFF for y in range(5) for x in range(7)],
              "a snapshot of 16 bits, pixel for pixel")
        out = dumped(open(os.path.join(work, "four.dgxi"), "rb").read())
        check(out["bits"] == 4 and values(out["pictures"][0]) == [((x + 2 * y) % 16) << 4 for y in range(3) for x in range(9)],
              "a snapshot of 4 bits, pixel for pixel")
        w, h, rgba, _ = d.read_png(open(os.path.join(work, "four.png"), "rb").read())
        check((w, h) == (9, 3) and tuple(rgba[4:8]) == (17, 17, 17, 255), "the PNG of a snapshot of 4 bits, its levels as grays")
        # a spoiled character: the CRC tells, nothing is written, and the tool says so
        spoiled = list(lines)
        k = spoiled.index("DGXSNAP BEGIN four") + 1
        spoiled[k] = ("B" if spoiled[k][0] != "B" else "C") + spoiled[k][1:]
        open(log, "w").write("\n".join(spoiled))
        other = os.path.join(work, "again")
        r = subprocess.run([sys.executable, "-I", tool, "--log", log, "--out", other], capture_output=True, text=True)
        check(r.returncode == 1 and "four spoiled" in r.stderr and not os.path.exists(os.path.join(other, "four.dgxi"))
              and os.path.isfile(os.path.join(other, "sixteen.dgxi")), "a spoiled snapshot is told and not written: %s" % r.stderr)
    finally:
        shutil.rmtree(work)


def test_refusals():
    one = png(1, 1, [(0, 0, 0, 255)])
    refused(lambda: d.make([one], bits=2), ["BITS 2", "1, 4, 8, 12, 16, 18, 24 or 32"])
    refused(lambda: d.make([one], bits=16, kind="indexed"), ["INDEXED 16 bits", "1, 4 or 8"])
    refused(lambda: d.make([one], bits=4, ink="dark"), ["INK at 4 bits"])
    refused(lambda: d.make([one], bits=16, alpha=True), ["ALPHA of pictures of pixels"])
    refused(lambda: d.make([one], bits=8, alpha=True, background="000000"), ["ALPHA and BACKGROUND"])
    refused(lambda: d.make([one], background="12345"), ["BACKGROUND 12345", "rrggbb"])
    refused(lambda: d.make([one], origin=(40000, 0)), ["ORIGIN 40000 0"])
    refused(lambda: d.make([]), ["no PNG"])
    refused(lambda: d.make([b"GIF89a"]), ["not a PNG"])
    spoiled = bytearray(one)
    spoiled[20] ^= 1
    refused(lambda: d.make([bytes(spoiled)]), ["IHDR", "CRC"])
    refused(lambda: d.make([one[:-12]]), ["IEND"])
    wide = b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" + struct.pack(">IIBBBBB", 70000, 1, 8, 6, 0, 0, 0)
    wide += struct.pack(">I", zlib.crc32(wide[12:29]) & 0xFFFFFFFF) + struct.pack(">I", 0) + b"IEND" + struct.pack(">I", zlib.crc32(b"IEND"))
    refused(lambda: d.make([wide]), ["70000x1", "65535"])
    refused(lambda: d.make([os.path.join(HERE, "no-such.png")]), ["no-such.png"])


# ---- needs ----

def tool(*args, cwd=None):
    run = subprocess.run([sys.executable, "-I", os.path.join(DGX, "tools", "dgx_images.py")] + list(args), capture_output=True, text=True, cwd=cwd)
    return run.returncode, run.stdout, run.stderr


def test_needs():
    work = tempfile.mkdtemp(prefix="dgxi-")
    try:
        os.makedirs(os.path.join(work, "art", "sub dir"))
        open(os.path.join(work, "art", "on.png"), "wb").write(png(2, 2, [(255, 255, 255, 255), (0, 0, 0, 0)] * 2))
        open(os.path.join(work, "art", "off.png"), "wb").write(png(2, 2, [(0, 0, 0, 0)] * 3 + [(255, 255, 255, 255)]))
        open(os.path.join(work, "art", "sub dir", "logo.png"), "wb").write(png(3, 2, gradient(3, 2, holes=True)))
        open(os.path.join(work, "more.txt"), "w").write("photo PNG \"art/sub dir/logo.png\" BITS 32\n")
        needs = os.path.join(work, "images.txt")
        open(needs, "w").write("# the pictures of the program\n\n"
                               "wifi PNG art/on.png art/off.png BITS 1   # two states\n"
                               "logo PNG 'art/sub dir/logo.png' BITS 16 ORIGIN 1 -1\n"
                               "needs more.txt\n")
        code, _, err = tool("check", needs, "--depends", os.path.join(work, "deps.txt"))
        check(code == 0, "check: %s" % err)
        deps = open(os.path.join(work, "deps.txt")).read().split("\n")
        check(["need wifi", "need logo", "need photo"] == [x for x in deps if x.startswith("need ")]
              and "png " + os.path.join(work, "art", "on.png") in deps and "png " + os.path.join(work, "art", "sub dir", "logo.png") in deps
              and "needs " + os.path.join(work, "more.txt") in deps and "needs " + needs in deps, "check --depends names everything: %s" % deps)
        out = os.path.join(work, "out")
        code, _, err = tool("build", needs, out)
        check(code == 0, "build: %s" % err)
        for f in ("wifi.dgxi", "logo.dgxi", "photo.dgxi", "dgx_images-files.txt", "dgx_assets.S", "dgx_assets.h"):
            check(os.path.isfile(os.path.join(out, f)), "build makes %s" % f)
        check(open(os.path.join(out, "dgx_images-files.txt")).read() == "wifi wifi.dgxi\nlogo logo.dgxi\nphoto photo.dgxi\n", "the list of files")
        made = open(os.path.join(out, "wifi.dgxi"), "rb").read()
        check(made == d.make([os.path.join(work, "art", "on.png"), os.path.join(work, "art", "off.png")], bits=1, name="wifi"),
              "a need of a file makes what make() makes")
        logo = dumped(open(os.path.join(out, "logo.dgxi"), "rb").read())
        check(logo["pictures"][0]["origin"] == (1, -1) and logo["bits"] == 16, "the words of a need: BITS, ORIGIN")
        # dgx_assets.S assembled and linked on the host: every file at an address divisible by 4, its bytes and size
        test_c = os.path.join(work, "assets_test.c")
        open(test_c, "w").write(
            '#include <stdio.h>\n#include <stdint.h>\n#include <string.h>\n#include "dgx_assets.h"\n'
            "static int same(const uint8_t *a, uint32_t n, const char *path) {\n"
            '  FILE *f = fopen(path, "rb"); uint8_t b[65536]; size_t m = fread(b, 1, sizeof b, f); fclose(f);\n'
            "  return ((uintptr_t)a % 4 == 0) && m == n && !memcmp(a, b, n); }\n"
            "int main(int argc, char **argv) { (void)argc;\n"
            "  return !(same(dgx_asset_wifi, dgx_asset_wifi_size, argv[1]) && same(dgx_asset_logo, dgx_asset_logo_size, argv[2]) &&\n"
            "           same(dgx_asset_photo, dgx_asset_photo_size, argv[3])); }\n")
        cc = shutil.which("cc") or shutil.which("gcc")
        if cc:
            exe = os.path.join(work, "assets_test")
            run = subprocess.run([cc, "-Wall", "-Wextra", "-I", out, "-o", exe, test_c, os.path.join(out, "dgx_assets.S")], capture_output=True, text=True)
            check(run.returncode == 0, "dgx_assets.S assembles and links: %s" % run.stderr)
            if run.returncode == 0:
                r = subprocess.run([exe] + [os.path.join(out, n + ".dgxi") for n in ("wifi", "logo", "photo")])
                check(r.returncode == 0, "every embedded file is at an address divisible by 4, of its bytes and size")
        else:
            print("note: no C compiler, dgx_assets.S is not assembled")
        # assets of the lists of images and fonts: a font named by its file, one file once, a name of two files refused
        fonts = os.path.join(work, "fonts")
        os.makedirs(fonts)
        open(os.path.join(fonts, "Small9.dgxf"), "wb").write(b"DGXF" + bytes(60))
        open(os.path.join(fonts, "dgx_fonts-files.txt"), "w").write("Small9 Small9.dgxf\ndgx_font_digits Small9.dgxf\n")
        both = os.path.join(work, "both")
        code, _, err = tool("assets", both, "--images", os.path.join(out, "dgx_images-files.txt"), "--fonts", os.path.join(fonts, "dgx_fonts-files.txt"))
        check(code == 0, "assets of images and fonts: %s" % err)
        head = open(os.path.join(both, "dgx_assets.h")).read()
        check("dgx_asset_Small9[]" in head and "dgx_asset_wifi[]" in head and head.count("dgx_asset_Small9[]") == 1, "assets: a font by its file, once")
        asm = open(os.path.join(both, "dgx_assets.S")).read()
        open(os.path.join(fonts, "Small9.dgxf"), "wb").write(b"DGXF" + bytes(59) + b"\1")
        tool("assets", both, "--images", os.path.join(out, "dgx_images-files.txt"), "--fonts", os.path.join(fonts, "dgx_fonts-files.txt"))
        check(open(os.path.join(both, "dgx_assets.S")).read() != asm, "a file changed in its bytes, not its size, changes dgx_assets.S")
        open(os.path.join(fonts, "dgx_fonts-files.txt"), "w").write("wifi wifi.dgxf\n")
        open(os.path.join(fonts, "wifi.dgxf"), "wb").write(b"DGXF")
        code, _, err = tool("assets", both, "--images", os.path.join(out, "dgx_images-files.txt"), "--fonts", os.path.join(fonts, "dgx_fonts-files.txt"))
        check(code == 1 and "dgx_asset_wifi is the name of two files" in err, "a picture and a font of one name refused: %s" % err)
        code, _, _ = tool("build", needs, os.path.join(work, "only"), "--no-assets")
        check(code == 0 and not os.path.exists(os.path.join(work, "only", "dgx_assets.S")), "build --no-assets leaves the assets out")
        # a need taken away takes its file away; nothing is written that has not changed
        open(needs, "w").write("wifi PNG art/on.png art/off.png BITS 1\n")
        before = os.stat(os.path.join(out, "wifi.dgxi")).st_mtime_ns
        code, _, err = tool("build", needs, out)
        check(code == 0 and not os.path.exists(os.path.join(out, "logo.dgxi")), "a need taken away takes its file away")
        check(os.stat(os.path.join(out, "wifi.dgxi")).st_mtime_ns == before, "a file that has not changed is not written")
        # the mistakes of a file of needs, each told with the file and the line
        mistakes = [
            ("wifi PNG art/on.png BITS 2\n", ":1:", "BITS 2"),
            ("\n\nwifi PNG art/none.png\n", ":3:", "none.png does not exist"),
            ("wifi PNG art/on.png SIZE 3\n", ":1:", "not a word of a need: 'SIZE'"),
            ("wifi PNG art/on.png BITS 4 SIZE 3\n", ":1:", "not a word of a need: 'SIZE'"),
            ("2wifi PNG art/on.png\n", ":1:", "C identifier"),
            ("wifi PNG art/on.png\nwifi PNG art/off.png\n", ":2:", "taken by the need at"),
            ("wifi BITS 4\n", ":1:", "no picture"),
            ("wifi GIF none.gif\n", ":1:", "none.gif does not exist"),
            ("wifi GIF art/on.png DELAY 100\n", ":1:", "DELAY of a GIF"),
            ("wifi PNG 'art/on.png BITS 4\n", ":1:", "closing quotation"),
            ("wifi PNG art/on.png BITS 4 BITS 8\n", ":1:", "BITS given twice"),
            ("wifi PNG art/on.png BITS 4 MASK INDEXED\n", ":1:", "a picture is of one kind"),
            ("wifi PNG art/on.png BITS 1 MASK\n", ":1:", "MASK of 1 bits"),
            ("wifi PNG art/on.png INK DARK\n", ":1:", "INK at 16 bits"),
            ("wifi PNG art/on.png ORIGIN 1\n", ":1:", "ORIGIN without x and y"),
            ("wifi FROM CMakeLists.txt PNG art/on.png BITS 2\n", "CMakeLists.txt: ", "BITS 2"),
            ("photo PNG art/on.png\nneeds more.txt\n", "more.txt:1:", "taken by the need at"),
        ]
        for text, where, what in mistakes:
            open(needs, "w").write(text)
            code, _, err = tool("check", needs)
            check(code == 1 and where in err and what in err, "a file of needs %r: '%s' and '%s' in %r" % (text, where, what, err))
        # make on the command line, paths from where it runs
        code, _, err = tool("make", os.path.join(work, "one.dgxi"), "PNG", "art/on.png", "BITS", "4", cwd=work)
        check(code == 0 and dumped(open(os.path.join(work, "one.dgxi"), "rb").read())["bits"] == 4, "make on the command line: %s" % err)
    finally:
        shutil.rmtree(work)


def main():
    if not os.path.isfile(DUMP):
        print("FAIL: no %s: build it with make -C test/images" % DUMP)
        return 1
    test_pngsuite()
    test_depths()
    test_colors()
    test_mask()
    test_gif()
    test_back_to_png()
    test_snapshots()
    test_refusals()
    test_needs()
    if failures:
        print("%d check(s) failed" % failures)
        return 1
    print("test_images: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
