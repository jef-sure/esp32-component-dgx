#!/usr/bin/env python3
"""Writes main/sprites.c: the pictures of the demo drawn as text, packed as dgx_image_blit() takes them.

Run it after changing a picture: python3 make_sprites.py
"""
import math

# a walker of 16 x 24, 4 bits: the letter is the value, '.' is 0, transparent
VALUES = {'.': 0, 'k': 1, 's': 2, 'h': 3, 'H': 4, 't': 5, 'T': 6, 'b': 7, 'l': 8, 'o': 9, 'e': 10}
BODY = [
    "......kkkk......",
    ".....khhhHk.....",
    "....khhhhhHk....",
    "....khhhhhhk....",
    "....kkkkkkkk....",
    ".....ksssesk....",
    ".....ksssssk....",
    "......kssk......",
    ".....kttttk.....",
    "....kttttttk....",
    "...ksttTtttsk...",
    "...ksttTtttsk...",
    "...ksttTtttsk...",
    "....kbbbbbbk....",
    "....ktttTttk....",
    "....kttTTttk....",
]
STAND = [
    ".....kllkllk....",
    ".....kllkllk....",
    ".....kllkllk....",
    ".....kllkllk....",
    ".....kllkllk....",
    "....kooo.kooo...",
    "....kkkk.kkkk...",
    "................",
]
STRIDE = [
    "....kll..kll....",
    "....kll..kll....",
    "...kll....kll...",
    "...kll....kll...",
    "..kll......kll..",
    ".kooo.....kooo..",
    ".kkkk.....kkkk..",
    "................",
]
# an arrow of 16 x 16, 1 bit
ARROW = [
    "................",
    "........#.......",
    "........##......",
    "........###.....",
    "........####....",
    "#############...",
    "##############..",
    "###############.",
    "###############.",
    "##############..",
    "#############...",
    "........####....",
    "........###.....",
    "........##......",
    "........#.......",
    "................",
]


def pack(rows, bits, value):
    out = []
    for row in rows:
        acc, n = 0, 0
        for ch in row:
            acc = (acc << bits) | value(ch)
            n += bits
            if n == 8:
                out.append(acc)
                acc, n = 0, 0
        if n:
            out.append(acc << (8 - n))
    return out


def array(name, data, art):
    s = "/*\n" + "".join(" * " + line + "\n" for line in art) + " */\n"
    s += "const uint8_t %s[%d] = {\n" % (name, len(data))
    for i in range(0, len(data), 16):
        s += "    " + ", ".join("0x%02x" % d for d in data[i:i + 16]) + ",\n"
    return s + "};\n\n"


def coin():
    """A coin of 12 x 12, 8 bits: its values are indices of the frame, COIN_FIRST .. COIN_FIRST + 5, dark to light."""
    rows, art = [], []
    for y in range(12):
        row, line = [], ""
        for x in range(12):
            dx, dy = x - 5.5, y - 5.5
            r = math.hypot(dx, dy)
            if r > 5.8:
                row.append(0)
                line += "."
                continue
            # light from the upper left, a dark rim
            shade = 0 if r > 4.9 else max(1, min(5, int(3.5 - (dx + dy) / 3.0)))
            row.append(COIN_FIRST + shade)
            line += str(shade)
        rows.append(row)
        art.append(line)
    return [v for row in rows for v in row], art


COIN_FIRST = 130

for rows in (BODY, STAND, STRIDE, ARROW):
    for r in rows:
        assert len(r) == 16, r

walker = lambda ch: VALUES[ch]
src = """/*
 * The pictures of the demo, written by make_sprites.py from the drawings
 * above each of them. Do not edit by hand.
 */
#include "sprites.h"

"""
src += array("walker_stand_data", pack(BODY + STAND, 4, walker), BODY + STAND)
src += array("walker_stride_data", pack(BODY + STRIDE, 4, walker), BODY + STRIDE)
src += array("arrow_data", pack(ARROW, 1, lambda ch: ch == '#'), ARROW)
coin_data, coin_art = coin()
src += array("coin_data", coin_data, coin_art)
open("main/sprites.c", "w").write(src)
