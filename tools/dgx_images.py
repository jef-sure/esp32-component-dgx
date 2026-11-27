#!/usr/bin/env python3
"""Pictures of PNG files as a .dgxi file of DGX, and the pictures a program needs, made when it is built.

    dgx_images.py make OUT.dgxi PNG <file>... [words]   one file of a set of pictures
    dgx_images.py check NEEDS [--depends FILE]          the needs of a file checked; FILE gets what they name
    dgx_images.py build NEEDS OUTDIR [--no-assets]      the files of the needs, their list, dgx_assets.S and .h
    dgx_images.py png IN.dgxi OUT.png [INDEX]           a picture of a file as a PNG, as DGX draws it
    dgx_images.py assets OUTDIR [--images LIST] [--fonts LIST]
                                                        dgx_assets.S and .h of the files of the lists of images and
                                                        fonts, dgx_images-files.txt and dgx_fonts-files.txt

The words of a picture are those dgx_image_need() takes in CMake and a line of a file of needs takes:

    PNG <file>...        the pictures of the set, in this order
    GIF <file>           or the frames of one GIF, each whole, with their delays and how many times it is played
    DELAY <ms>           a set of PNG pictures as an animation, every picture shown so long
    LOOPS <n>            how many times an animation is played, 0 for ever; the GIF's own, or 0, if not given
    BITS <n>             1, 4, 8, 12, 16, 18, 24 or 32; 16 if not given
    INDEXED | PIXELS | MASK
                         the kind: 1, 4 and 8 bits are indexed unless PIXELS or MASK is given, the others are
                         pixels; a mask, of 4 or 8 bits, is how much of one color covers each pixel, for soft icons
    REDUCE               an indexed set of more colors than its depth holds is reduced to them, by median cut
    DITHER               the error of every pixel spread over its neighbours, Floyd-Steinberg
    INK DARK | LIGHT     at 1 bit, and for a mask of a PNG without transparency, whether the dark or the light pixels
                         are the ink; LIGHT if not given
    ALPHA                an indexed set keeps the opacity of the PNG in the entries of its palette
    BACKGROUND <rrggbb>  the transparent places painted in this color, mixed by their opacity
    ORIGIN <x> <y>       the origin of every picture of the set; 0 0 if not given

A file of needs has a need a line: its name, then its words, paths from the directory of the file, a path with a
space in quotes. A line "needs <file>" takes the needs of another file; # starts a comment. An error is told with
the file and the number of its line.

As a module, for a program that has the PNG pictures itself:

    from dgx_images import make, DgxiError
    data = make(["on.png", "off.png"], bits=4)       # paths, or the bytes of each PNG

A refusal is a DgxiError with the words the build prints. Nothing is read but the PNG pictures given.

The format of a file is told in src/vscreen/image_file.c, the reader of DGX. Python 3.8 or later, its own library.
"""
import hashlib
import os
import shlex
import struct
import sys
import zlib

FORMAT = 1
FORMAT_MASK = 2  # a mask is of format 2: a DGX that reads only format 1 refuses it as of a later DGX
INDEXED = 1
PIXELS = 2
MASK = 3
DEPTHS = (1, 4, 8, 12, 16, 18, 24, 32)
HEAD = 64
PICTURE = 16
MOST = 65535  # the width and the height of a picture of DGX are 16-bit numbers
TEXT_KEYS = ("Title", "Author", "Copyright", "Description", "Source")


class DgxiError(Exception):
    """A picture or a need DGX does not take; the words say why."""


# ---- PNG ----

_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_DEPTHS_OF_TYPE = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8), 4: (8, 16), 6: (8, 16)}
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
_ADAM7 = ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2))


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _unfilter(data, at, width, height, bpp, stride):
    """the rows of a picture, each stride bytes, from at; returns them and where the next picture begins"""
    rows = []
    prior = bytearray(stride)
    for _ in range(height):
        if at + 1 + stride > len(data):
            raise DgxiError("the picture data of the PNG is cut")
        kind = data[at]
        row = bytearray(data[at + 1:at + 1 + stride])
        at += 1 + stride
        if kind == 1:
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                row[i] = (row[i] + prior[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + ((left + prior[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up_left = prior[i - bpp] if i >= bpp else 0
                row[i] = (row[i] + _paeth(left, prior[i], up_left)) & 0xFF
        elif kind != 0:
            raise DgxiError("a row of the PNG has filter %d, which PNG does not have" % kind)
        rows.append(row)
        prior = row
    return rows, at


def _samples(row, count, depth, channels):
    """the samples of a row, as numbers of the depth of the file"""
    n = count * channels
    if depth == 8:
        return list(row[:n])
    if depth == 16:
        return [row[2 * i] << 8 | row[2 * i + 1] for i in range(n)]
    per_byte = 8 // depth
    mask = (1 << depth) - 1
    out = []
    for i in range(n):
        byte = row[i // per_byte]
        shift = 8 - depth * (i % per_byte + 1)
        out.append(byte >> shift & mask)
    return out


def read_png(data):
    """width, height, the pixels as RGBA bytes row after row, and the texts of the PNG, a dict"""
    if not isinstance(data, (bytes, bytearray)) or data[:8] != _SIGNATURE:
        raise DgxiError("not a PNG: no signature of one")
    at = 8
    head = None
    palette = None
    transparent = None
    idat = []
    texts = {}
    ended = False
    while at < len(data):
        if at + 12 > len(data):
            raise DgxiError("the PNG is cut in a chunk")
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        if at + 12 + length > len(data):
            raise DgxiError("the PNG is cut in its %s chunk" % kind.decode("latin-1", "replace"))
        body = data[at + 8:at + 8 + length]
        crc = struct.unpack(">I", data[at + 8 + length:at + 12 + length])[0]
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise DgxiError("the %s chunk of the PNG is spoiled: its CRC does not match" % kind.decode("latin-1", "replace"))
        at += 12 + length
        if kind == b"IHDR":
            if head is not None or length != 13:
                raise DgxiError("the PNG has a spoiled IHDR chunk")
            head = struct.unpack(">IIBBBBB", body)
        elif head is None:
            raise DgxiError("the PNG does not start with its IHDR chunk")
        elif kind == b"PLTE":
            if length % 3 or not 1 <= length // 3 <= 256:
                raise DgxiError("the PNG has a palette of a length no PNG has")
            palette = [tuple(body[i:i + 3]) for i in range(0, length, 3)]
        elif kind == b"tRNS":
            transparent = bytes(body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            ended = True
            break
        elif kind in (b"tEXt", b"zTXt", b"iTXt"):
            _text(kind, body, texts)
        elif kind[0] & 0x20 == 0:
            raise DgxiError("the PNG has a chunk %s that must be understood, and DGX does not know it" % kind.decode("latin-1", "replace"))
    if head is None:
        raise DgxiError("the PNG has no IHDR chunk")
    if not ended:
        raise DgxiError("the PNG is cut: it has no IEND chunk")
    width, height, depth, color, compression, filtering, interlace = head
    if not width or not height:
        raise DgxiError("the PNG has no pixels")
    if width > MOST or height > MOST:
        raise DgxiError("a picture of %dx%d: DGX takes pictures of up to %d pixels a side" % (width, height, MOST))
    if color not in _DEPTHS_OF_TYPE or depth not in _DEPTHS_OF_TYPE[color]:
        raise DgxiError("the PNG has color type %d of %d bits, which PNG does not have" % (color, depth))
    if compression or filtering or interlace > 1:
        raise DgxiError("the PNG has a compression, filtering or interlace method PNG does not have")
    if color == 3 and palette is None:
        raise DgxiError("the PNG of palette colors has no palette")
    try:
        raw = zlib.decompress(b"".join(idat))
    except zlib.error as e:
        raise DgxiError("the picture data of the PNG is spoiled: %s" % e)
    channels = _CHANNELS[color]
    bpp = max(1, channels * depth // 8)
    pixels = bytearray(width * height * 4)
    passes = _ADAM7 if interlace else ((0, 0, 1, 1),)
    at = 0
    for x0, y0, dx, dy in passes:
        w = (width - x0 + dx - 1) // dx
        h = (height - y0 + dy - 1) // dy
        if w <= 0 or h <= 0:
            continue
        stride = (w * channels * depth + 7) // 8
        rows, at = _unfilter(raw, at, w, h, bpp, stride)
        for j, row in enumerate(rows):
            s = _samples(row, w, depth, channels)
            y = y0 + j * dy
            for i in range(w):
                x = x0 + i * dx
                o = (y * width + x) * 4
                pixels[o:o + 4] = _rgba(color, depth, s[i * channels:(i + 1) * channels], palette, transparent)
    return width, height, pixels, texts


def _to8(v, depth):
    """a sample of the depth of the file as 8 bits, the nearest"""
    if depth == 8:
        return v
    if depth == 16:
        return (v * 255 + 32767) // 65535
    return v * 255 // ((1 << depth) - 1)


def _rgba(color, depth, s, palette, transparent):
    if color == 3:
        index = s[0]
        if index >= len(palette):
            raise DgxiError("a pixel of the PNG is an index past the end of its palette")
        r, g, b = palette[index]
        a = transparent[index] if transparent is not None and index < len(transparent) else 255
        return bytes((r, g, b, a))
    if color == 0:
        g = _to8(s[0], depth)
        a = 0 if transparent is not None and len(transparent) >= 2 and s[0] == struct.unpack(">H", transparent[:2])[0] else 255
        return bytes((g, g, g, a))
    if color == 2:
        a = 0 if transparent is not None and len(transparent) >= 6 and tuple(s) == struct.unpack(">HHH", transparent[:6]) else 255
        return bytes((_to8(s[0], depth), _to8(s[1], depth), _to8(s[2], depth), a))
    if color == 4:
        g = _to8(s[0], depth)
        return bytes((g, g, g, _to8(s[1], depth)))
    return bytes(_to8(v, depth) for v in s)


def _text(kind, body, texts):
    """a text of the PNG kept by its key, for the text about the pictures; one that cannot be read is left out"""
    try:
        key, rest = body.split(b"\0", 1)
        key = key.decode("latin-1")
        if kind == b"tEXt":
            value = rest.decode("latin-1")
        elif kind == b"zTXt":
            value = zlib.decompress(rest[1:]).decode("latin-1")
        else:
            compressed, rest = rest[0], rest[2:]
            _, rest = rest.split(b"\0", 1)
            _, rest = rest.split(b"\0", 1)
            value = (zlib.decompress(rest) if compressed else rest).decode("utf-8")
    except (ValueError, IndexError, zlib.error, UnicodeDecodeError):
        return
    if key in TEXT_KEYS and value.strip():
        texts.setdefault(key, [])
        if value.strip() not in texts[key]:
            texts[key].append(value.strip())


# ---- GIF ----

def _sub_blocks(data, at):
    """the data sub-blocks of a GIF from at, joined, and where they end"""
    out = bytearray()
    while True:
        if at >= len(data):
            raise DgxiError("the GIF is cut in its data")
        size = data[at]
        at += 1
        if not size:
            return bytes(out), at
        if at + size > len(data):
            raise DgxiError("the GIF is cut in its data")
        out += data[at:at + size]
        at += size


def _lzw(data, minimum, count):
    """count indices of the LZW data of a GIF frame, codes of minimum + 1 bits and more, the low bits first"""
    if not 2 <= minimum <= 8:
        raise DgxiError("a frame of the GIF has a code size of %d, which GIF does not have" % minimum)
    clear, end = 1 << minimum, (1 << minimum) + 1
    first = [bytes((i,)) for i in range(clear)] + [b"", b""]
    table, size, prior = list(first), minimum + 1, None
    out = bytearray()
    buffer = bits = at = 0
    while len(out) < count:
        while bits < size:
            if at >= len(data):
                return bytes(out) + bytes(count - len(out))  # cut short, as many GIFs are: the rest is the first color
            buffer |= data[at] << bits
            at += 1
            bits += 8
        code = buffer & ((1 << size) - 1)
        buffer >>= size
        bits -= size
        if code == clear:
            table, size, prior = list(first), minimum + 1, None
            continue
        if code == end:
            break
        if prior is None:
            if code >= clear:
                raise DgxiError("the picture data of the GIF is spoiled")
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            if len(table) < 4096:
                table.append(prior + entry[:1])
        elif code == len(table):
            entry = prior + prior[:1]
            if len(table) < 4096:
                table.append(entry)
        else:
            raise DgxiError("the picture data of the GIF is spoiled")
        out += entry
        prior = entry
        if len(table) == 1 << size and size < 12:
            size += 1
    return bytes(out[:count]) + bytes(max(0, count - len(out)))


def _deinterlace(indices, w, h):
    rows = [y for start, step in ((0, 8), (4, 8), (2, 4), (1, 2)) for y in range(start, h, step)]
    out = bytearray(w * h)
    for k, y in enumerate(rows):
        out[y * w:(y + 1) * w] = indices[k * w:(k + 1) * w]
    return bytes(out)


def read_gif(data):
    """width, height, its frames whole as RGBA bytes, the delay of every frame in milliseconds, and how many times it is
    played, 0 for ever. A frame is drawn over the one before as GIF says, the disposal of each frame applied; the
    canvas starts transparent. A delay of 0 or 10 ms is played as 100 ms, as browsers play it."""
    if not isinstance(data, (bytes, bytearray)) or data[:6] not in (b"GIF87a", b"GIF89a"):
        raise DgxiError("not a GIF: no signature of one")
    if len(data) < 13:
        raise DgxiError("the GIF is cut in its head")
    w, h, packed = struct.unpack("<HHB", data[6:11])
    at = 13
    shared = None
    if packed & 0x80:
        n = 3 << ((packed & 7) + 1)
        if at + n > len(data):
            raise DgxiError("the GIF is cut in its color table")
        shared = [tuple(data[at + i:at + i + 3]) for i in range(0, n, 3)]
        at += n
    if not w or not h:
        raise DgxiError("the GIF has no pixels")
    canvas = bytearray(w * h * 4)
    frames, delays, loops, control = [], [], 1, None
    while at < len(data):
        block = data[at]
        at += 1
        if block == 0x3B:
            break
        if block == 0x21:
            if at >= len(data):
                raise DgxiError("the GIF is cut in an extension")
            label = data[at]
            body, at = _sub_blocks(data, at + 1)
            if label == 0xF9 and len(body) >= 4:
                control = ((body[0] >> 2) & 7, body[1] | body[2] << 8, body[3] if body[0] & 1 else None)
            elif label == 0xFF and body[:11] in (b"NETSCAPE2.0", b"ANIMEXTS1.0") and len(body) >= 14 and body[11] == 1:
                loops = body[12] | body[13] << 8
            continue
        if block != 0x2C:
            raise DgxiError("the GIF has a block 0x%02x, which GIF does not have" % block)
        if at + 9 > len(data):
            raise DgxiError("the GIF is cut in a frame")
        left, top, fw, fh, p = struct.unpack("<HHHHB", data[at:at + 9])
        at += 9
        table = shared
        if p & 0x80:
            n = 3 << ((p & 7) + 1)
            if at + n > len(data):
                raise DgxiError("the GIF is cut in the color table of a frame")
            table = [tuple(data[at + i:at + i + 3]) for i in range(0, n, 3)]
            at += n
        if table is None:
            raise DgxiError("a frame of the GIF has no color table")
        if at >= len(data):
            raise DgxiError("the GIF is cut in a frame")
        minimum = data[at]
        packed_data, at = _sub_blocks(data, at + 1)
        indices = _lzw(packed_data, minimum, fw * fh)
        if p & 0x40:
            indices = _deinterlace(indices, fw, fh)
        disposal, delay, clear_index = control or (0, 0, None)
        before = bytes(canvas) if disposal == 3 else None
        for y in range(min(fh, h - top) if top < h else 0):
            for x in range(min(fw, w - left) if left < w else 0):
                i = indices[y * fw + x]
                if i == clear_index:
                    continue
                if i >= len(table):
                    raise DgxiError("a pixel of the GIF is an index past the end of its color table")
                o = ((top + y) * w + left + x) * 4
                canvas[o:o + 4] = bytes(table[i]) + b"\xff"
        frames.append(bytearray(canvas))
        delays.append(delay * 10 if delay > 1 else 100)
        if disposal == 2:
            for y in range(top, min(h, top + fh)):
                canvas[(y * w + left) * 4:(y * w + min(w, left + fw)) * 4] = bytes(4 * max(0, min(w, left + fw) - left))
        elif disposal == 3:
            canvas[:] = before
        control = None
    if not frames:
        raise DgxiError("the GIF has no frame")
    if w > MOST or h > MOST:
        raise DgxiError("a picture of %dx%d: DGX takes pictures of up to %d pixels a side" % (w, h, MOST))
    return w, h, frames, delays, loops


# ---- colors ----

def _gray(r, g, b):
    """the brightness DGX gives a color on a screen of fewer bits, dgx_screen_rgb()"""
    return (r * 77 + g * 150 + b * 29) >> 8


def _level(v, top):
    """the nearest of the levels 0..top of a value 0..255"""
    return (v * top * 2 + 255) // 510


def _high(v, bits):
    """the high `bits` of a value 0..255: the level DGX_RGB_16() and dgx_screen_rgb() give, so that a color of a
    picture is the same color the program writes"""
    return v >> (8 - bits)


def _value(q, top):
    """the value 0..255 of the level q of 0..top"""
    return (q * 510 + top) // (2 * top)


def _spread(work, w, h, x, y, err, channels):
    """the error of a pixel spread over its neighbours to the right and below, Floyd-Steinberg"""
    for dx, dy, part in ((1, 0, 7), (-1, 1, 3), (0, 1, 5), (1, 1, 1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < w and ny < h:
            o = (ny * w + nx) * channels
            for c in range(channels):
                work[o + c] += err[c] * part / 16.0


def _median_cut(counts, wanted, channels):
    """at most wanted colors standing for the colors of counts, {color: pixels}: boxes split at the median of their widest channel"""
    def measured(box):
        """a box with its widest channel and how wide it is, counted once"""
        widths = [max(c[k] for c, _ in box) - min(c[k] for c, _ in box) for k in range(channels)]
        k = max(range(channels), key=lambda j: widths[j])
        return [widths[k] if len(box) > 1 else 0, k, box]

    boxes = [measured(sorted(counts.items()))]
    while len(boxes) < wanted:
        best = max(range(len(boxes)), key=lambda i: boxes[i][0])
        if boxes[best][0] == 0:
            break
        _, k, box = boxes.pop(best)
        box.sort(key=lambda item: (item[0][k], item[0]))
        total = sum(n for _, n in box)
        run, cut = 0, len(box) - 1
        for i, (_, n) in enumerate(box[:-1]):
            run += n
            if run * 2 >= total:
                cut = i + 1
                break
        boxes += [measured(box[:cut]), measured(box[cut:])]
    out = []
    for _, _, box in boxes:
        total = sum(n for _, n in box)
        out.append(tuple((sum(c[k] * n for c, n in box) + total // 2) // total for k in range(channels)))
    return out


class _Nearest:
    """the nearest entry of a palette to a color, remembered by the cell of 8 levels a channel the color falls in"""

    def __init__(self, entries):
        self.entries = entries
        self.exact = {e: i for i, e in reversed(list(enumerate(entries)))}
        self.cells = {}

    def __call__(self, color):
        i = self.exact.get(color)
        if i is not None:
            return i
        key = tuple(c >> 3 for c in color)
        i = self.cells.get(key)
        if i is None:
            centre = tuple((c << 3) + 4 for c in key)
            i = min(range(len(self.entries)), key=lambda j: sum((a - b) * (a - b) for a, b in zip(self.entries[j], centre)))
            self.cells[key] = i
        return i


# ---- a set of pictures ----

def _hex_color(text):
    t = text[1:] if text.startswith("#") else text
    if len(t) != 6 or any(ch not in "0123456789abcdefABCDEF" for ch in t):
        raise DgxiError("BACKGROUND %s: a color is six hexadecimal digits, rrggbb" % text)
    return (int(t[0:2], 16), int(t[2:4], 16), int(t[4:6], 16))


def make(pngs=(), bits=16, kind=None, reduce=False, dither=False, ink=None, background=None, alpha=False, origin=(0, 0),
         name=None, sources=None, gif=None, delay=None, loops=None):
    """The bytes of a .dgxi file of the PNG pictures given, by their paths or as their bytes, in this order, or of the
    frames of one GIF, by its path or as its bytes.

    kind is "indexed", "pixels" or "mask", or None for the kind of the depth; ink is "dark" or "light"; background
    is "rrggbb" or (r, g, b); name and sources, the names of the pictures, go into the text about them. An animation
    keeps the delay of every picture in milliseconds and how many times it is played, 0 for ever: a GIF its own, a
    set of PNG pictures `delay` each, `loops` times, 0 for ever if not given.
    """
    if bits not in DEPTHS:
        raise DgxiError("BITS %s: a picture takes 1, 4, 8, 12, 16, 18, 24 or 32 bits" % bits)
    if kind is None:
        kind = "indexed" if bits <= 8 else "pixels"
    if kind not in ("indexed", "pixels", "mask"):
        raise DgxiError("the kind %r: a picture is INDEXED, PIXELS or MASK" % (kind,))
    if ink is not None and ink not in ("dark", "light"):
        raise DgxiError("INK %s: ink is DARK or LIGHT" % ink)
    if isinstance(background, str):
        background = _hex_color(background)
    make_check([], bits, kind, reduce, dither, ink, background, alpha, origin, gif, delay, loops)
    ox, oy = origin
    if not (-32768 <= ox <= 32767 and -32768 <= oy <= 32767):
        raise DgxiError("ORIGIN %d %d: an origin is a 16-bit number with a sign" % (ox, oy))
    if gif is not None and pngs:
        raise DgxiError("PNG and GIF: a set is made of PNG pictures or of the frames of one GIF")
    if gif is None and not pngs:
        raise DgxiError("no PNG picture given")
    if len(pngs) > MOST:
        raise DgxiError("%d pictures: a file holds up to %d" % (len(pngs), MOST))
    pictures, texts, names = [], {}, []
    delays = None
    if gif is not None:
        label = sources[0] if sources else (os.path.basename(gif) if isinstance(gif, (str, os.PathLike)) else "the GIF")
        names.append(str(label))
        try:
            if isinstance(gif, (bytes, bytearray)):
                data = gif
            else:
                with open(gif, "rb") as f:
                    data = f.read()
            w, h, frames, delays, gif_loops = read_gif(data)
        except OSError as e:
            raise DgxiError("%s: %s" % (gif, e.strerror or e))
        except DgxiError as e:
            raise DgxiError("%s: %s" % (label, e))
        if len(frames) > MOST:
            raise DgxiError("%s: %d frames: a file holds up to %d" % (label, len(frames), MOST))
        loops = gif_loops if loops is None else loops
        pictures = [(w, h, rgba) for rgba in frames]
    elif delay is not None:
        delays = [delay] * len(pngs)
        loops = 0 if loops is None else loops
    for k, png in enumerate(pngs):
        label = sources[k] if sources and k < len(sources) else (os.path.basename(png) if isinstance(png, (str, os.PathLike)) else "picture %d" % (k + 1))
        names.append(str(label))
        if isinstance(png, (bytes, bytearray)):
            data = png
        else:
            try:
                with open(png, "rb") as f:
                    data = f.read()
            except OSError as e:
                raise DgxiError("%s: %s" % (png, e.strerror or e))
        try:
            w, h, rgba, t = read_png(data)
        except DgxiError as e:
            raise DgxiError("%s: %s" % (label, e))
        for key, values in t.items():
            for v in values:
                texts.setdefault(key, [])
                if v not in texts[key]:
                    texts[key].append(v)
        pictures.append((w, h, rgba))
    if background is not None:
        br, bg, bb = background
        for _, _, rgba in pictures:
            for o in range(0, len(rgba), 4):
                a = rgba[o + 3]
                if a != 255:
                    rgba[o] = (rgba[o] * a + br * (255 - a) + 127) // 255
                    rgba[o + 1] = (rgba[o + 1] * a + bg * (255 - a) + 127) // 255
                    rgba[o + 2] = (rgba[o + 2] * a + bb * (255 - a) + 127) // 255
                    rgba[o + 3] = 255
    if kind == "indexed":
        palette, data = _indexed(pictures, bits, reduce, dither, ink, alpha)
        planes = [None] * len(pictures)
        kind_code = INDEXED
    elif kind == "mask":
        palette, data = _mask(pictures, bits, dither, ink)
        planes = [None] * len(pictures)
        kind_code = MASK
    else:
        palette = []
        data, planes = _pixels(pictures, bits, dither, ink)
        kind_code = PIXELS
    about = ["name: %s" % name] if name else []
    about += ["made-by: tools/dgx_images.py of DGX, format %d" % FORMAT, "source: %s" % ", ".join(names)]
    about += ["%s: %s" % (key, "; ".join(texts[key])) for key in TEXT_KEYS if key in texts]
    return _write(bits, kind_code, pictures, palette, data, planes, (ox, oy), "\n".join(about).encode("utf-8"), delays, loops)


def _indexed(pictures, bits, reduce, dither, ink, alpha):
    """the palette, entries (r, g, b, a), and the values of the pixels of every picture"""
    full = 1 << bits

    def opaque_of(a):
        return a > 0 if alpha else a >= 128

    has_transparent = any(not opaque_of(rgba[o]) for _, _, rgba in pictures for o in range(3, len(rgba), 4))
    if bits == 1:
        return _one_bit(pictures, has_transparent, dither, ink, alpha, opaque_of)
    channels = 4 if alpha else 3
    counts, order = {}, []
    for _, _, rgba in pictures:
        for o in range(0, len(rgba), 4):
            if opaque_of(rgba[o + 3]):
                c = tuple(rgba[o:o + channels]) if alpha else tuple(rgba[o:o + 3])
                if c not in counts:
                    counts[c] = 0
                    order.append(c)
                counts[c] += 1
    room = full - 1 if has_transparent else full
    if len(order) <= room:
        entries = order
    elif reduce:
        entries = _median_cut(counts, room, channels)
    else:
        raise DgxiError("%d colors, %d bits hold %d%s: REDUCE, or fewer colors in the picture"
                        % (len(order), bits, room, " with transparency" if has_transparent else ""))
    if not entries:
        entries = [(0, 0, 0, 255)[:channels]]
    first = 1 if has_transparent or len(entries) < full else 0
    nearest = _Nearest(entries)
    exact = len(entries) == len(order)
    values = []
    for w, h, rgba in pictures:
        v = bytearray(w * h)
        if dither and not exact:
            work = [float(x) for o in range(0, len(rgba), 4) for x in rgba[o:o + channels]]
        for y in range(h):
            for x in range(w):
                p = y * w + x
                if not opaque_of(rgba[p * 4 + 3]):
                    continue
                if dither and not exact:
                    c = tuple(min(255, max(0, int(round(work[p * channels + k])))) for k in range(channels))
                    i = nearest(c)
                    e = entries[i]
                    _spread(work, w, h, x, y, [c[k] - e[k] for k in range(channels)], channels)
                else:
                    c = tuple(rgba[p * 4:p * 4 + channels])
                    i = nearest(c)
                v[p] = i + first
        values.append(v)
    palette = [tuple(e[:3]) + ((e[3],) if alpha else (255,)) for e in entries]
    return palette, values


def _mask(pictures, bits, dither, ink):
    """the mean color it covers, the one entry of the palette, and how much of the ink covers every pixel: the opacity of
    the PNG, or, in a set without transparent pixels, the brightness, the light the ink unless INK DARK"""
    top = (1 << bits) - 1
    by_alpha = any(rgba[o] != 255 for _, _, rgba in pictures for o in range(3, len(rgba), 4))
    values, sums = [], [0, 0, 0, 0]
    for w, h, rgba in pictures:
        cover = [rgba[o + 3] if by_alpha else _gray(*rgba[o:o + 3]) for o in range(0, len(rgba), 4)]
        if not by_alpha and ink == "dark":
            cover = [255 - c for c in cover]
        work = [float(c) for c in cover]
        v = bytearray(w * h)
        for p in range(w * h):
            c = min(255, max(0, int(round(work[p]))))
            q = _level(c, top)
            if dither:
                _spread(work, w, h, p % w, p // w, [c - _value(q, top)], 1)
            v[p] = q
            o = p * 4
            for k in range(3):
                sums[k] += rgba[o + k] * cover[p]
            sums[3] += cover[p]
        values.append(v)
    n = sums[3]
    entry = tuple((sums[k] + n // 2) // n for k in range(3)) if n else (255, 255, 255)
    return [entry + (255,)], values


def _one_bit(pictures, has_transparent, dither, ink, alpha, opaque_of):
    """1 bit: with transparency the opaque pixels are 1, of the mean color; without, the light or the dark ones"""
    values = []
    sums = {0: [0, 0, 0, 0, 0], 1: [0, 0, 0, 0, 0]}  # r, g, b, a, pixels
    for w, h, rgba in pictures:
        v = bytearray(w * h)
        if not has_transparent and dither:
            work = [float(_gray(*rgba[o:o + 3])) for o in range(0, len(rgba), 4)]
        for p in range(w * h):
            o = p * 4
            if has_transparent:
                bit = 1 if opaque_of(rgba[o + 3]) else 0
            else:
                if dither:
                    g = min(255, max(0, int(round(work[p]))))
                    light = 1 if g >= 128 else 0
                    _spread(work, w, h, p % w, p // w, [g - (255 if light else 0)], 1)
                else:
                    light = 1 if _gray(*rgba[o:o + 3]) >= 128 else 0
                bit = light if ink != "dark" else 1 - light
            v[p] = bit
            s = sums[bit]
            for k in range(4):
                s[k] += rgba[o + k]
            s[4] += 1
        values.append(v)

    def mean(bit):
        s = sums[bit]
        n = s[4]
        return tuple((s[k] + n // 2) // n for k in range(3)) + (((s[3] + n // 2) // n) if alpha else 255,)

    if has_transparent:
        return [mean(1) if sums[1][4] else (0, 0, 0, 255)], values
    if sums[0][4] and sums[1][4]:
        return [mean(0), mean(1)], values
    # one color only: every pixel 1, as 0 would be transparent in a palette of one
    only = 1 if sums[1][4] else 0
    return [mean(only)], [bytearray(b"\x01" * len(v)) for v in values]


_CHANNEL_BITS = {12: (4, 4, 4), 16: (5, 6, 5), 18: (6, 6, 6), 24: (8, 8, 8), 32: (8, 8, 8)}


def _pixels(pictures, bits, dither, ink):
    """the values of the pixels of every picture, as a virtual screen of the depth holds them, and their planes of opacity"""
    data, planes = [], []
    for w, h, rgba in pictures:
        n = w * h
        if bits <= 8:
            top = (1 << bits) - 1
            work = [float(_gray(*rgba[o:o + 3])) for o in range(0, len(rgba), 4)]
            vals = []
            for p in range(n):
                g = min(255, max(0, int(round(work[p]))))
                q = _high(g, bits)
                if dither:
                    _spread(work, w, h, p % w, p // w, [g - _value(q, top)], 1)
                if ink == "dark":
                    q = top - q
                vals.append(q)
            data.append(_pack_pixels(vals, bits))
        else:
            widths = _CHANNEL_BITS[bits]
            tops = [(1 << c) - 1 for c in widths]
            work = [float(x) for o in range(0, len(rgba), 4) for x in rgba[o:o + 3]] if dither else None
            out = bytearray()
            pending = None
            for p in range(n):
                if dither:
                    c = [min(255, max(0, int(round(work[p * 3 + k])))) for k in range(3)]
                else:
                    c = list(rgba[p * 4:p * 4 + 3])
                q = [_high(c[k], widths[k]) for k in range(3)]
                if dither:
                    _spread(work, w, h, p % w, p // w, [c[k] - _value(q[k], tops[k]) for k in range(3)], 3)
                r, g, b = q
                if bits == 12:
                    if pending is None:
                        out += bytes((r << 4 | g,))
                        pending = b << 4
                    else:
                        out += bytes((pending | r, g << 4 | b))
                        pending = None
                elif bits == 16:
                    v = r << 11 | g << 5 | b
                    out += bytes((v >> 8, v & 0xFF))
                elif bits == 18:
                    out += bytes((r << 2, g << 2, b << 2))
                elif bits == 24:
                    out += bytes((r, g, b))
                else:
                    out += bytes((rgba[p * 4 + 3], r, g, b))
            if pending is not None:
                out += bytes((pending,))
            data.append(bytes(out))
        if bits != 32 and any(rgba[o] != 255 for o in range(3, len(rgba), 4)):
            planes.append(bytes(rgba[3::4]))
        else:
            planes.append(None)
    return data, planes


def _pack_pixels(vals, bits):
    """values of 1, 4 or 8 bits one after another, the first in the high bits of a byte, as a virtual screen keeps them"""
    if bits == 8:
        return bytes(vals)
    per = 8 // bits
    out = bytearray((len(vals) + per - 1) // per)
    for i, v in enumerate(vals):
        out[i // per] |= v << (8 - bits * (i % per + 1))
    return bytes(out)


def _pack_rows(vals, w, h, bits):
    """values of 1, 4 or 8 bits, every row from a new byte, the leftmost pixel in the high bits, as dgx_image_t keeps them"""
    if bits == 8:
        return bytes(vals)
    row = (w * bits + 7) // 8
    per = 8 // bits
    out = bytearray(row * h)
    for y in range(h):
        for x in range(w):
            out[y * row + x // per] |= vals[y * w + x] << (8 - bits * (x % per + 1))
    return bytes(out)


def _align(n):
    return (n + 3) & ~3


def _write(bits, kind, pictures, palette, data, planes, origin, about, delays=None, loops=0):
    count = len(pictures)
    table = HEAD
    at = _align(table + count * PICTURE)
    palette_at = at if kind != PIXELS else 0
    if kind != PIXELS:
        at = _align(at + len(palette) * 4)
    # an animation: the delay of every picture, 2 bytes each, in the reserved part of the head where a DGX before
    # 0.12.12 does not look: it reads the pictures of the file as a set, without their times
    delays_at = 0
    if delays is not None:
        delays_at = at
        at = _align(at + 2 * count)
    parts = []
    entries = bytearray()
    for (w, h, _), values, plane in zip(pictures, data, planes):
        body = _pack_rows(values, w, h, bits) if kind != PIXELS else values
        pixels_at = at
        at = _align(at + len(body))
        plane_at = 0
        if plane is not None:
            plane_at = at
            at = _align(at + len(plane))
        entries += struct.pack("<HHhhII", w, h, origin[0], origin[1], pixels_at, plane_at)
        parts.append((pixels_at, body))
        if plane is not None:
            parts.append((plane_at, plane))
    about_at = at
    length = about_at + len(about)
    head = struct.pack("<4sHBBHHIIIII", b"DGXI", FORMAT_MASK if kind == MASK else FORMAT, bits, kind, count, len(palette) if kind != PIXELS else 0,
                       length, table, palette_at, about_at if about else 0, len(about))
    out = bytearray(length)
    out[0:len(head)] = head
    if delays is not None:
        out[32:38] = struct.pack("<IH", delays_at, min(65535, loops or 0))
        for k, ms in enumerate(delays):
            out[delays_at + 2 * k:delays_at + 2 * k + 2] = struct.pack("<H", max(1, min(65535, int(ms))))
    out[table:table + len(entries)] = entries
    if kind != PIXELS:
        flat = bytes(x for e in palette for x in e)
        out[palette_at:palette_at + len(flat)] = flat
    for offset, body in parts:
        out[offset:offset + len(body)] = body
    out[about_at:about_at + len(about)] = about
    return bytes(out)


# ---- a file read back, a PNG written ----

def write_png(w, h, rgba):
    """a PNG of RGBA pixels, 8 bits a channel"""
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    return _SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def read_dgxi(data):
    """a .dgxi file as {bits, kind, palette, delays, loops, pictures: [(w, h, origin_x, origin_y, rgba)]}, every picture
    in the colors it is drawn in: an indexed one by its palette, its 0 transparent unless the palette is full; a mask
    as the color of its palette covering by its values; pixels by their depth and their plane of opacity"""
    if len(data) < HEAD or data[:4] != b"DGXI":
        raise DgxiError("not a file of pictures of DGX: no mark DGXI")
    version, bits, kind, count, colors, length, table, palette_at, _, _ = struct.unpack("<HBBHHIIIII", data[4:32])
    if version not in (1, 2):
        raise DgxiError("a file of pictures of format %d: this reads formats 1 and 2" % version)
    if length > len(data):
        raise DgxiError("the file of pictures is cut")
    delays_at, loops = struct.unpack("<IH", data[32:38])
    palette = [tuple(data[palette_at + 4 * i:palette_at + 4 * i + 4]) for i in range(colors)] if kind != PIXELS else []
    first = 1 if kind == INDEXED and colors < 1 << bits else 0
    out = {"bits": bits, "kind": kind, "palette": palette, "loops": loops, "pictures": [],
           "delays": [struct.unpack("<H", data[delays_at + 2 * k:delays_at + 2 * k + 2])[0] for k in range(count)] if delays_at else None}
    for k in range(count):
        w, h, ox, oy, pixels, plane = struct.unpack("<HHhhII", data[table + PICTURE * k:table + PICTURE * (k + 1)])
        rgba = bytearray(w * h * 4)
        if kind != PIXELS:
            row = (w * bits + 7) // 8
            for y in range(h):
                for x in range(w):
                    o = (y * w + x) * 4
                    if bits == 8:
                        v = data[pixels + y * row + x]
                    else:
                        per = 8 // bits
                        v = data[pixels + y * row + x // per] >> (8 - bits * (x % per + 1)) & ((1 << bits) - 1)
                    if kind == MASK:
                        r, g, b, _ = palette[0]
                        rgba[o:o + 4] = bytes((r, g, b, v * 255 // ((1 << bits) - 1)))
                    elif v >= first and v - first < len(palette):
                        rgba[o:o + 4] = bytes(palette[v - first])
        else:
            n = w * h
            for i in range(n):
                o = i * 4
                if bits in (1, 4):
                    bit = i * bits
                    v = data[pixels + bit // 8] >> (8 - bits - bit % 8) & ((1 << bits) - 1)
                    g = v * 255 // ((1 << bits) - 1)
                    c = (g, g, g, 255)
                elif bits == 8:
                    g = data[pixels + i]
                    c = (g, g, g, 255)
                elif bits == 12:
                    at = pixels + i * 3 // 2
                    if i % 2 == 0:
                        r, gg, b = data[at] >> 4, data[at] & 15, data[at + 1] >> 4
                    else:
                        r, gg, b = data[at] & 15, data[at + 1] >> 4, data[at + 1] & 15
                    c = (r * 17, gg * 17, b * 17, 255)
                elif bits == 16:
                    v = data[pixels + 2 * i] << 8 | data[pixels + 2 * i + 1]
                    r, gg, b = v >> 11, v >> 5 & 63, v & 31
                    c = (r << 3 | r >> 2, gg << 2 | gg >> 4, b << 3 | b >> 2, 255)
                elif bits in (18, 24):
                    r, gg, b = data[pixels + 3 * i:pixels + 3 * i + 3]
                    if bits == 18:
                        r, gg, b = (r & 0xFC) | r >> 6, (gg & 0xFC) | gg >> 6, (b & 0xFC) | b >> 6
                    c = (r, gg, b, 255)
                else:
                    a, r, gg, b = data[pixels + 4 * i:pixels + 4 * i + 4]
                    c = (r, gg, b, a)
                if plane:
                    c = c[:3] + (data[plane + i],)
                rgba[o:o + 4] = bytes(c)
        out["pictures"].append((w, h, ox, oy, rgba))
    return out


def to_png(data, index=0):
    """the picture `index` of a .dgxi file as a PNG"""
    pictures = read_dgxi(data)["pictures"]
    if not 0 <= index < len(pictures):
        raise DgxiError("picture %d of a file of %d" % (index, len(pictures)))
    w, h, _, _, rgba = pictures[index]
    return write_png(w, h, rgba)


# ---- needs ----

KEYWORDS = ("PNG", "GIF", "BITS", "INDEXED", "PIXELS", "MASK", "REDUCE", "DITHER", "INK", "ALPHA", "BACKGROUND", "ORIGIN", "DELAY",
            "LOOPS", "FROM")


def _is_c_name(name):
    return bool(name) and (name[0].isalpha() or name[0] == "_") and all(ch.isalnum() or ch == "_" for ch in name) and name.isascii()


def parse_words(words, base):
    """the arguments of make() of the words of a need after its name, paths from the directory base"""
    args = {"pngs": []}
    seen = set()
    i = 0
    while i < len(words):
        w = words[i]
        if w not in KEYWORDS:
            raise DgxiError("not a word of a need: '%s'" % w)
        if w in seen:
            raise DgxiError("%s given twice" % w)
        seen.add(w)
        i += 1

        def value(what):
            nonlocal i
            if i >= len(words) or words[i] in KEYWORDS:
                raise DgxiError("%s without %s" % (w, what))
            i += 1
            return words[i - 1]

        if w == "PNG":
            while i < len(words) and words[i] not in KEYWORDS:
                path = os.path.normpath(os.path.join(base, words[i]))
                if words[i].isalpha() and words[i].isupper() and not os.path.exists(path):
                    raise DgxiError("not a word of a need: '%s', nor a file" % words[i])
                args["pngs"].append(path)
                i += 1
            if not args["pngs"]:
                raise DgxiError("PNG without a file")
        elif w == "GIF":
            args["gif"] = os.path.normpath(os.path.join(base, value("a file")))
        elif w in ("DELAY", "LOOPS"):
            v = value("a number")
            if not v.isdigit() or not (1 if w == "DELAY" else 0) <= int(v) <= 65535:
                raise DgxiError("%s %s: %s" % (w, v, "a delay is 1 to 65535 milliseconds" if w == "DELAY" else "loops are 0, for ever, to 65535"))
            args[w.lower()] = int(v)
        elif w == "BITS":
            v = value("a number")
            if not v.isdigit() or int(v) not in DEPTHS:
                raise DgxiError("BITS %s: a picture takes 1, 4, 8, 12, 16, 18, 24 or 32 bits" % v)
            args["bits"] = int(v)
        elif w in ("INDEXED", "PIXELS", "MASK"):
            if sum(k in seen for k in ("INDEXED", "PIXELS", "MASK")) > 1:
                raise DgxiError("%s: a picture is of one kind, INDEXED, PIXELS or MASK" % w)
            args["kind"] = w.lower()
        elif w in ("REDUCE", "DITHER", "ALPHA"):
            args[w.lower()] = True
        elif w == "INK":
            v = value("DARK or LIGHT")
            if v not in ("DARK", "LIGHT"):
                raise DgxiError("INK %s: ink is DARK or LIGHT" % v)
            args["ink"] = v.lower()
        elif w == "BACKGROUND":
            args["background"] = _hex_color(value("a color"))
        elif w == "ORIGIN":
            xs = [value("x and y"), value("x and y")]
            try:
                args["origin"] = (int(xs[0]), int(xs[1]))
            except ValueError:
                raise DgxiError("ORIGIN %s %s: an origin is two whole numbers" % tuple(xs))
        elif w == "FROM":
            args["where"] = value("a place")
    if not args["pngs"] and "gif" not in args:
        raise DgxiError("no picture: PNG <file>... or GIF <file>")
    return args


def read_needs(path, needs=None, files=None, seen=None):
    """the needs of a file of needs and of the files it takes: [(name, where, args)], and every file they name"""
    needs = [] if needs is None else needs
    files = [] if files is None else files
    seen = set() if seen is None else seen
    path = os.path.abspath(path)
    if path in seen:
        raise DgxiError("%s: the needs of this file are taken twice" % path)
    seen.add(path)
    files.append(path)
    base = os.path.dirname(path)
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise DgxiError("%s: %s" % (path, e.strerror or e))
    names = {n: w for n, w, _ in needs}
    for number, line in enumerate(lines, 1):
        where = "%s:%d" % (path, number)
        try:
            lex = shlex.shlex(line, posix=True)
            lex.whitespace_split = True
            lex.commenters = "#"
            lex.escape = ""
            words = list(lex)
        except ValueError as e:
            raise DgxiError("%s: %s" % (where, e))
        if not words:
            continue
        try:
            if words[0] == "needs":
                if len(words) != 2:
                    raise DgxiError("needs takes one file")
                read_needs(os.path.join(base, words[1]), needs, files, seen)
                names = {n: w for n, w, _ in needs}
                continue
            name = words[0]
            # FROM <file>: the need was given by dgx_image_need() in that CMakeLists, which errors name instead of this line
            if "FROM" in words[1:-1]:
                where = "%s: dgx_image_need(%s)" % (words[words.index("FROM", 1) + 1], name)
            if not _is_c_name(name):
                raise DgxiError("'%s': the name of a need is a C identifier" % name)
            args = parse_words(words[1:], base)
            where = args.pop("where", where)
            if name in names:
                raise DgxiError("%s: the name is taken by the need at %s" % (name, names[name]))
            for png in args["pngs"] + ([args["gif"]] if "gif" in args else []):
                if not os.path.isfile(png):
                    raise DgxiError("%s does not exist" % png)
            make_check(**args)
        except DgxiError as e:
            raise DgxiError("%s: %s" % (where, e))
        needs.append((name, where, args))
        names[name] = where
        files.extend(args["pngs"] + ([args["gif"]] if "gif" in args else []))
    return needs, files


def make_check(pngs, bits=16, kind=None, reduce=False, dither=False, ink=None, background=None, alpha=False, origin=(0, 0), gif=None,
               delay=None, loops=None):
    """the words of a need checked as make() checks them, without reading a picture"""
    if gif is not None and pngs:
        raise DgxiError("PNG and GIF: a set is made of PNG pictures or of the frames of one GIF")
    if gif is not None and delay is not None:
        raise DgxiError("DELAY of a GIF: a GIF has the delays of its frames")
    if gif is None and delay is None and loops is not None:
        raise DgxiError("LOOPS without DELAY: a set of PNG pictures is an animation by DELAY")
    if kind is None:
        kind = "indexed" if bits <= 8 else "pixels"
    if kind == "indexed" and bits > 8:
        raise DgxiError("INDEXED %d bits: an indexed picture takes 1, 4 or 8 bits" % bits)
    if kind == "mask" and bits not in (4, 8):
        raise DgxiError("MASK of %d bits: a mask takes 4 or 8 bits" % bits)
    if ink is not None and bits != 1 and kind != "mask":
        raise DgxiError("INK at %d bits: ink says which pixels are 1, at 1 bit, or which are the ink of a mask" % bits)
    if alpha and kind == "mask":
        raise DgxiError("ALPHA of a mask: a mask is the opacity of its PNG already")
    if alpha and kind != "indexed":
        raise DgxiError("ALPHA of pictures of pixels: they keep their opacity in a plane of their own, or BACKGROUND paints it over")
    if background is not None and kind == "mask":
        raise DgxiError("BACKGROUND of a mask: a mask is made of the transparency BACKGROUND would paint over")
    if alpha and background is not None:
        raise DgxiError("ALPHA and BACKGROUND: BACKGROUND paints over the opacity ALPHA keeps")
    if not (-32768 <= origin[0] <= 32767 and -32768 <= origin[1] <= 32767):
        raise DgxiError("ORIGIN %d %d: an origin is a 16-bit number with a sign" % origin)


def _write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode("utf-8")
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(data)


def _asm_string(path):
    return '"%s"' % os.path.abspath(path).replace("\\", "/").replace('"', '\\"')


def write_assets(outdir, assets):
    """dgx_assets.S and dgx_assets.h of outdir: every file of assets, [(name, path)], embedded as dgx_asset_<name>"""
    names = {}
    for name, path in assets:
        if not _is_c_name(name):
            raise DgxiError("%s: dgx_asset_%s is no C name" % (path, name))
        if name in names:
            raise DgxiError("dgx_asset_%s is the name of two files, %s and %s" % (name, names[name], path))
        names[name] = path
    asm = ["/* The files DGX embeds, each at an address divisible by 4 and in a section of its own, which the linker",
           " * leaves out when the program does not name it. Made by tools/dgx_images.py; do not edit. The digest",
           " * of every file is here so that this text changes, and is assembled again, when a file does. */", ""]
    head = ["#pragma once", "/* The files DGX embeds, see dgx_image_need(). Made by tools/dgx_images.py; do not edit. */",
            "#include <stdint.h>", "", "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for name, path in assets:
        with open(path, "rb") as f:
            content = f.read()
        size = len(content)
        symbol = "dgx_asset_" + name
        asm += ["/* %s, %d bytes, sha256 %s */" % (os.path.basename(path), size, hashlib.sha256(content).hexdigest()[:16]),
                "#if defined(__APPLE__)",
                "    .const_data",
                "    .balign 4",
                "    .globl _%s" % symbol, "_%s:" % symbol,
                "    .incbin %s" % _asm_string(path),
                "    .balign 4",
                "    .globl _%s_size" % symbol, "_%s_size:" % symbol,
                "    .long %d" % size,
                "#else",
                '    .section .rodata.%s,"a"' % symbol,
                "    .balign 4",
                "    .global %s" % symbol, "%s:" % symbol,
                "    .incbin %s" % _asm_string(path),
                "    .balign 4",
                "    .global %s_size" % symbol, "%s_size:" % symbol,
                "    .long %d" % size,
                "#endif", ""]
        head += ["/** %s, %d bytes */" % (os.path.basename(path), size),
                 "extern const uint8_t  %s[];" % symbol,
                 "extern const uint32_t %s_size;" % symbol, ""]
    asm += ["#if defined(__linux__) && defined(__ELF__)", '    .section .note.GNU-stack,"",%progbits', "#endif", ""]
    head += ["#ifdef __cplusplus", "}", "#endif", ""]
    _write_if_changed(os.path.join(outdir, "dgx_assets.S"), "\n".join(asm))
    _write_if_changed(os.path.join(outdir, "dgx_assets.h"), "\n".join(head))


def read_list(path):
    """the files of a list of dgx_images-files.txt or dgx_fonts-files.txt, a line "<name> <file>": [(stem, path)], a file once"""
    base = os.path.dirname(os.path.abspath(path))
    out, seen = [], set()
    with open(path, encoding="utf-8") as f:
        for line in f:
            words = line.split()
            if len(words) != 2:
                continue
            file = os.path.join(base, words[1])
            if file not in seen:
                seen.add(file)
                out.append((os.path.splitext(os.path.basename(file))[0], file))
    return out


def build(needs_path, outdir, assets=True):
    """every need of a file of needs made into outdir: <name>.dgxi, dgx_images-files.txt, dgx_assets.S and .h"""
    needs, _ = read_needs(needs_path)
    os.makedirs(outdir, exist_ok=True)
    listed, embedded = [], []
    for name, where, args in needs:
        try:
            data = make(name=name, **args)
        except DgxiError as e:
            raise DgxiError("%s: %s: %s" % (where, name, e))
        path = os.path.join(outdir, name + ".dgxi")
        _write_if_changed(path, data)
        listed.append("%s %s.dgxi" % (name, name))
        embedded.append((name, path))
    _write_if_changed(os.path.join(outdir, "dgx_images-files.txt"), "".join(line + "\n" for line in listed))
    if assets:
        write_assets(outdir, embedded)
    # a file made by a need taken away is taken away with it
    made = {name + ".dgxi" for name, _, _ in needs}
    for f in os.listdir(outdir):
        if f.endswith(".dgxi") and f not in made:
            os.remove(os.path.join(outdir, f))


def main(argv):
    try:
        if len(argv) >= 3 and argv[1] == "make":
            args = parse_words(argv[3:], os.getcwd())
            args.pop("where", None)
            data = make(**args)
            with open(argv[2], "wb") as f:
                f.write(data)
            return 0
        if len(argv) in (3, 5) and argv[1] == "check":
            needs, files = read_needs(argv[2])
            if len(argv) == 5:
                if argv[3] != "--depends":
                    raise DgxiError("check NEEDS [--depends FILE]")
                # for CMake: the name of every need, every file of needs, every PNG; a line each
                pngs = {p for _, _, a in needs for p in a["pngs"] + ([a["gif"]] if "gif" in a else [])}
                lines = ["need %s" % name for name, _, _ in needs]
                lines += ["%s %s" % ("png" if f in pngs else "needs", os.path.abspath(f).replace("\\", "/")) for f in files]
                _write_if_changed(argv[4], "".join(line + "\n" for line in lines))
            return 0
        if len(argv) in (4, 5) and argv[1] == "png":
            with open(argv[2], "rb") as f:
                data = f.read()
            png = to_png(data, int(argv[4]) if len(argv) == 5 else 0)
            with open(argv[3], "wb") as f:
                f.write(png)
            return 0
        if len(argv) in (4, 5) and argv[1] == "build" and (len(argv) == 4 or argv[4] == "--no-assets"):
            build(argv[2], argv[3], assets=len(argv) == 4)
            return 0
        if len(argv) >= 3 and argv[1] == "assets" and len(argv) % 2 == 1:
            lists = {"--images": None, "--fonts": None}
            for k in range(3, len(argv), 2):
                if argv[k] not in lists:
                    raise DgxiError("assets OUTDIR [--images LIST] [--fonts LIST]")
                lists[argv[k]] = argv[k + 1]
            os.makedirs(argv[2], exist_ok=True)
            write_assets(argv[2], [a for key in ("--images", "--fonts") if lists[key] for a in read_list(lists[key])])
            return 0
    except DgxiError as e:
        print("dgx_images: %s" % e, file=sys.stderr)
        return 1
    print(__doc__.strip(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
