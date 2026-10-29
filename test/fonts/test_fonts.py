#!/usr/bin/env python3
"""
The fonts of a program by its needs, font2c/dgx_fonts.py, on the host: the needs of this directory built into
dgx_fonts.c and dgx_fonts.h, the letters and the pairs checked, the C compiled as the host tests compile DGX, and the
mistakes a program can make told as they should be.

Run: make -C test/fonts. It needs what a program with needs needs: a C compiler and FreeType for font2c, fontconfig,
and the DejaVu fonts (DejaVu Sans and DejaVu Sans Mono) installed; without them it says so and checks nothing.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DGX = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(DGX, "font2c", "dgx_fonts.py")
STUBS = os.path.join(DGX, "host", "stubs")
failures = 0


def check(condition, what):
    global failures
    if not condition:
        failures += 1
        print(f"FAIL: {what}")


def tool(*args):
    run = subprocess.run([sys.executable, "-I", TOOL] + list(args), capture_output=True, text=True)
    return run.returncode, run.stdout, run.stderr


def build(work, needs, board=True, rules=True):
    """The tool run on these needs (a text, the words of the file after the component line); exit code, stdout, stderr."""
    head = f"component {DGX}\n"
    if board:
        head += "scale 4\nboard pixels 1280 800\nboard mm 217 136\n"
    if rules:
        head += f"rules {os.path.join(HERE, 'fonts.rules')}\n"
    path = os.path.join(work, "needs.txt")
    with open(path, "w", encoding="utf-8") as f:
        f.write(head + needs)
    return tool("build", path, os.path.join(work, "out"))


def have_dejavu():
    try:
        out = subprocess.run(["fc-list", ":postscriptname=DejaVuSans", "--format", "%{postscriptname}\n"], capture_output=True, text=True).stdout
        mono = subprocess.run(["fc-list", ":postscriptname=DejaVuSansMono", "--format", "%{postscriptname}\n"], capture_output=True, text=True).stdout
    except OSError:
        return False
    return "DejaVuSans" in out.split() and "DejaVuSansMono" in mono.split()


def test_build(work):
    po, keys, title = (os.path.join(HERE, f) for f in ("strings.po", "keys.json", "title.txt"))
    own = os.path.join(DGX, "src", "fonts", "TerminusTTFMedium12.c")  # as a font of the program's own
    code, out, err = build(work, f"""need title
size 4.5mm
text {title}
need text
size 12pt
mono 1
text {po}
need hand
dgx font0ss
text {po}
except {{}}
need time
file {own}
letters 0123456789:
need keys
size 4mm
postscript DejaVuSans
text {keys}
need same
size 4mm
postscript DejaVuSans
text {title}
""")
    check(code == 0, f"build: {err.strip()}")
    if code:
        return
    outdir = os.path.join(work, "out")
    h = open(os.path.join(outdir, "dgx_fonts.h"), encoding="utf-8").read()
    c = open(os.path.join(outdir, "dgx_fonts.c"), encoding="utf-8").read()
    for line in ("dgx_font_t *dgx_font_text(void);", "#define DGX_FONT_TEXT_MADE 1", "#define DGX_FONT_TEXT_PX 48", "#define DGX_FONT_HAND_MADE 0",
                 "#define DGX_FONT_TIME_MADE 0", "#define DGX_FONT_KEYS_MM 4", "#define DGX_FONT_TITLE_MM 4.5", "#define DGX_FONT_TITLE_PX 27",
                 "#define DGX_FONT_NEEDS(X) X(title) X(text) X(hand) X(time) X(keys) X(same)", "#define DGX_FONTS_BOARD_WIDTH_MM 217",
                 "#define DGX_FONTS_SCALE 4"):
        check(line in h, f"header has: {line}")
    # 4 mm at 1280/217 px/mm is 24 px: keys and same share one font of DejaVu Sans at 24 px
    check("#define DGX_FONT_KEYS_PX 24" in h and "#define DGX_FONT_SAME_PX 24" in h, "4 mm is 24 px on this board")
    arrays = re.findall(r"^static const uint8_t (\w+)_bitmaps\[\]", c, re.M)
    check(sorted(arrays) == ["DejaVuSansBook24", "DejaVuSansMonoBook27", "DejaVuSansMonoBook48"], f"three fonts made for six needs: {arrays}")
    check("dgx_font_t *dgx_font_same(void)\n{\n    return dgx_font_keys();\n}" in c, "keys and same share one font, same being a function of keys'")
    check("static const uint8_t bitmaps[]" not in c, "the arrays are named by the font")
    check('#include "fonts/font0ss.h"' in h and "dgx_font_t *TerminusTTFMedium12(void);" in h, "fonts as they are are declared in the header")
    check("#define dgx_font_hand font0ss" in h and "#define dgx_font_time TerminusTTFMedium12" in h, "fonts as they are are macros of their own functions")
    check("return font0ss();" not in c, "no adapter of a font as it is in the C: the library of DGX does not refer to the program")
    # the letters of the text need are exactly the letters of its .po, no ASCII thrown in
    code, letters, err = tool("letters", po)
    check(code == 0, f"letters: {err.strip()}")
    code, text, err = tool("json", os.path.join(outdir, "dgx_fonts.c"), "--font", "dgx_font_text")
    check(code == 0, f"json: {err.strip()}")
    if code == 0:
        glyphs = {chr(int(k)) for k in json.loads(text)["glyphs"]}
        check(glyphs == set(letters.strip("\n")), f"the glyphs of text are the letters of strings.po: {''.join(sorted(glyphs ^ set(letters.strip())))}")
        check("щ" in glyphs and "{" in glyphs and "A" not in glyphs, "Cyrillic and the braces of the strings in, no ASCII the strings have not")
    code, text, err = tool("json", os.path.join(outdir, "dgx_fonts.c"), "--font", "dgx_font_same")
    if code == 0:
        glyphs = {chr(int(k)) for k in json.loads(text)["glyphs"]}
        check({"a", "Q", "ё", "Й", "1", ","} <= glyphs, "the strings of the keyboard's JSON, ASCII and Russian, are in its font")
        check("Ä" in glyphs, "the letters of the other need of the pair are in it too")
    # the C compiles as the host tests compile DGX
    run = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-c", f"-I{STUBS}", f"-I{os.path.join(DGX, 'include')}",
                          "-include", os.path.join(STUBS, "esp_log.h"), os.path.join(outdir, "dgx_fonts.c"), "-o", os.path.join(outdir, "dgx_fonts.o")],
                         capture_output=True, text=True)
    check(run.returncode == 0, f"dgx_fonts.c compiles: {run.stderr.strip()}")
    # the same again: nothing rewritten
    code, out, err = tool("build", os.path.join(work, "needs.txt"), outdir)
    check(code == 0 and "as they were" in out, f"a second run leaves the files: {out.strip()} {err.strip()}")
    check(not os.path.exists(os.path.join(outdir, "work")), "the work of font2c is cleared")


def test_mistakes(work):
    po, keys, title = (os.path.join(HERE, f) for f in ("strings.po", "keys.json", "title.txt"))
    cases = [
        ("a font as it is without the letters", f"need hand\ndgx font0ss\ntext {po}\n", True, True, "lacks the characters"),
        ("a need by family without rules", f"need text\nsize 12pt\nmono 1\ntext {po}\n", True, False, "declared no rules"),
        ("a size in mm without the display", f"need title\nsize 4.5mm\ntext {title}\n", False, True, "needs the display"),
        ("a need named as a function of DGX", f"need string_bounds\nsize 12pt\ntext {title}\n", True, True, "is already a function of DGX"),
        ("an unknown PostScript name", f"need title\nsize 12pt\npostscript NoSuchFace-Regular\ntext {title}\n", True, True, "no installed face has the PostScript name"),
        ("an unknown font of DGX", f"need hand\ndgx NoSuchFont\n", True, True, "DGX has no font"),
        ("no letters at all", "need title\nsize 12pt\n", True, True, "no characters"),
        ("a need twice with different fonts", f"need title /a/CMakeLists.txt\nsize 12pt\ntext {title}\nneed title /b/CMakeLists.txt\nsize 14pt\ntext {title}\n",
         True, True, "declared twice with different fonts: DejaVu Sans Mono Book at 48 px in /a/CMakeLists.txt, DejaVu Sans Mono Book at 56 px in /b/CMakeLists.txt"),
        ("a font as it is with a size", f"need hand\ndgx font0ss\nsize 12pt\ntext {title}\n", True, True, "has its own size"),
        ("an exact face with mono", f"need title\nsize 12pt\npostscript DejaVuSans\nmono 1\ntext {title}\n", True, True, "postscript names the face itself; mono do not apply"),
        ("a font file with a family", f"need title\nsize 12pt\nsource /x.ttf\nfamily DejaVu*\ntext {title}\n", True, True, "source names the face itself; family do not apply"),
    ]
    for what, needs, board, rules, expected in cases:
        shutil.rmtree(os.path.join(work, "out"), ignore_errors=True)
        code, out, err = build(work, needs, board, rules)
        check(code == 1 and expected in err, f"{what}: told '{expected}', got exit {code}: {err.strip()}")


def test_needs_file(work):
    """A needs file written by hand, as a program built on the host keeps it: relative paths, no component line, the
    board and the rules as arguments."""
    rel = os.path.relpath(HERE, work)
    with open(os.path.join(work, "fonts.needs"), "w", encoding="utf-8") as f:
        f.write(f"""# the fonts of the program, for the board and for the host alike
need text
size 12pt
mono 1
text {rel}/strings.po
need keys
size 4mm
postscript DejaVuSans
text {rel}/keys.json
need hand
dgx font0ss
text {rel}/strings.po
except {{}}
""")
    out = os.path.join(work, "out-file")
    code, told, err = tool("build", os.path.join(work, "fonts.needs"), out, "--scale", "4", "--pixels", "1280", "800", "--mm", "217", "136",
                           "--rules", os.path.join(HERE, "fonts.rules"))
    check(code == 0, f"a needs file with the board as arguments: {err.strip()}")
    if code == 0:
        h = open(os.path.join(out, "dgx_fonts.h"), encoding="utf-8").read()
        check("#define DGX_FONT_TEXT_PX 48" in h and "#define DGX_FONT_KEYS_PX 24" in h and "#define DGX_FONTS_SCALE 4" in h,
              "the sizes and the board from the arguments")
        check("#define DGX_FONT_NEEDS(X) X(text) X(keys) X(hand)" in h, "the needs in the order of the file")
    code, told, err = tool("build", os.path.join(work, "fonts.needs"), out, "--pixels", "1280", "800", "--mm", "217", "136",
                           "--rules", os.path.join(HERE, "fonts.rules"))
    check(code == 0 and "#define DGX_FONT_TEXT_PX 12" in open(os.path.join(out, "dgx_fonts.h"), encoding="utf-8").read(),
          f"without a scale a point is a pixel: {err.strip()}")
    with open(os.path.join(work, "twice.needs"), "w", encoding="utf-8") as f:
        f.write("scale 4\nscale 3\nneed text\nsize 12pt\nletters abc\n")
    code, told, err = tool("build", os.path.join(work, "twice.needs"), out)
    check(code == 1 and "scale given twice" in err, f"a word twice with other values is told: {err.strip()}")
    with open(os.path.join(work, "again.needs"), "w", encoding="utf-8") as f:
        f.write("scale 4\nboard pixels 1280 800\nscale 4\nboard pixels 1280 800\nneed text\nsize 12pt\nletters abc\npostscript DejaVuSans\n")
    code, told, err = tool("build", os.path.join(work, "again.needs"), out)
    check(code == 0, f"the same board told again is no mistake: {err.strip()}")


def test_twice(work):
    """A need told in two places that come to one font is one need with the letters of both."""
    po, title = (os.path.join(HERE, f) for f in ("strings.po", "title.txt"))
    code, told, err = build(work, f"need title /a/CMakeLists.txt\nsize 12pt\ntext {title}\nneed title /b/CMakeLists.txt\nsize 12pt\ntext {po}\n"
                                  f"need hand /a/CMakeLists.txt\ndgx font0ss\nletters abc\nneed hand /b/CMakeLists.txt\ndgx font0ss\nletters xyz\n")
    check(code == 0, f"a need told twice with one font: {err.strip()}")
    if code == 0:
        h = open(os.path.join(work, "out", "dgx_fonts.h"), encoding="utf-8").read()
        check(h.count("dgx_font_t *dgx_font_title(void);") == 1 and "#define DGX_FONT_NEEDS(X) X(title) X(hand)" in h, "one need in the header")
        code, text, err = tool("json", os.path.join(work, "out", "dgx_fonts.c"), "--font", "dgx_font_title")
        if code == 0:
            glyphs = {chr(int(k)) for k in json.loads(text)["glyphs"]}
            check("Ä" in glyphs and "щ" in glyphs, "the letters of both places are in the font")


def test_lists(work):
    """A list of fonts for font2c -jo given as it is beside the needs: its functions in the same header, and the same
    list run through font2c alone."""
    with open(os.path.join(work, "own.list"), "w", encoding="utf-8") as f:
        f.write("# the fonts of a component, as it names them\n"
                "-n widgets_title /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf 24 0x20 0x7e 0xc4 0xdc\n"
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf 30 0x30 0x39\n")
    needs = f"need keys\nsize 4mm\npostscript DejaVuSans\ntext {os.path.join(HERE, 'keys.json')}\nlist own.list\n"
    with open(os.path.join(work, "with-list.needs"), "w", encoding="utf-8") as f:
        f.write(needs)
    out = os.path.join(work, "out-list")
    code, told, err = tool("build", os.path.join(work, "with-list.needs"), out, "--pixels", "1280", "800", "--mm", "217", "136")
    check(code == 0, f"a needs file with a list: {err.strip()}")
    if code == 0:
        h = open(os.path.join(out, "dgx_fonts.h"), encoding="utf-8").read()
        c = open(os.path.join(out, "dgx_fonts.c"), encoding="utf-8").read()
        check("dgx_font_t *widgets_title(void);" in h and "dgx_font_t *dgx_fonts_DejaVuSansBook30(void);" in h, "the functions of the list are declared")
        check("#define DGX_FONT_NEEDS(X) X(keys)" in h, "the list's fonts are no needs")
        # keys and widgets_title are one face at 24 px: one font, two functions
        check(len(re.findall(r"^static const uint8_t DejaVuSansBook24_bitmaps\[\]", c, re.M)) == 1 and "return dgx_font_keys();" in c,
              "a font of the list and a need of the same face and size are one font")
        check("widgets_title: from a list" in told, f"the list's fonts are told: {told.strip()}")
    # the list alone through font2c, as a project without the needs may run it
    run = subprocess.run([os.path.join(out, "font2c"), "-jo", os.path.join(work, "own.list"), "-o", os.path.join(work, "alone")],
                         capture_output=True, text=True)
    check(run.returncode == 0 and os.path.exists(os.path.join(work, "alone.h")), f"font2c -jo alone: {run.stderr.strip()}")
    if run.returncode == 0:
        h = open(os.path.join(work, "alone.h"), encoding="utf-8").read()
        check("dgx_font_t *widgets_title(void);" in h and "dgx_font_t *alone_DejaVuSansBook30(void);" in h, "the functions of the list alone")


def bdf_font(path, px):
    """A bitmap font of BDF, of its one size px: the letters A and B, frames of 10 x px pixels."""
    rows = "".join("FFC0\n" if y in (0, px - 1) else "8040\n" for y in range(px))
    chars = "".join(f"STARTCHAR {c}\nENCODING {ord(c)}\nSWIDTH 500 0\nDWIDTH 11 0\nBBX 10 {px} 0 -4\nBITMAP\n{rows}ENDCHAR\n" for c in "AB")
    with open(path, "w", encoding="ascii") as f:
        f.write(f"STARTFONT 2.1\nFONT -test-Testbdf-Medium-R-Normal--{px}-{px * 10}-75-75-C-100-ISO10646-1\nSIZE {px} 75 75\n"
                f"FONTBOUNDINGBOX 10 {px} 0 -4\nSTARTPROPERTIES 7\nFAMILY_NAME \"Testbdf\"\nPIXEL_SIZE {px}\nFONT_ASCENT {px - 4}\n"
                f"FONT_DESCENT 4\nCHARSET_REGISTRY \"ISO10646\"\nCHARSET_ENCODING \"1\"\nCOPYRIGHT \"Copyright (c) 2026 the test\"\n"
                f"ENDPROPERTIES\nCHARS 2\n{chars}ENDFONT\n")


def test_bitmap_font_in_a_list(work):
    """A list may name any font file font2c reads, a bitmap font of BDF at its own size too, with what it says of its
    copyright in fonts-used.txt; at a size it has not, font2c says which it has."""
    bdf_font(os.path.join(work, "test20.bdf"), 20)
    with open(os.path.join(work, "bdf.list"), "w", encoding="utf-8") as f:
        f.write("-n bitmap_font test20.bdf 20 0x41 0x42\n")
    with open(os.path.join(work, "bdf.needs"), "w", encoding="utf-8") as f:
        f.write(f"component {DGX}\nlist bdf.list\n")
    out = os.path.join(work, "out-bdf")
    code, told, err = tool("build", os.path.join(work, "bdf.needs"), out)
    check(code == 0, f"a BDF in a list: {err.strip()}")
    if code == 0:
        h = open(os.path.join(out, "dgx_fonts.h"), encoding="utf-8").read()
        used = open(os.path.join(out, "fonts-used.txt"), encoding="utf-8").read()
        check("dgx_font_t *bitmap_font(void);" in h and glyphs_of(os.path.join(out, "dgx_fonts.c"), "bitmap_font") == {"A", "B"},
              "the function of the BDF is declared and has its two letters")
        check("copyright: Copyright (c) 2026 the test" in used, f"fonts-used.txt has the COPYRIGHT of the BDF: {used}")
    # a size the bitmaps have not: font2c names the sizes they have, where it made the font of their own size under the
    # name of the size asked for
    with open(os.path.join(work, "bdf.list"), "w", encoding="utf-8") as f:
        f.write("-n bitmap_font test20.bdf 12 0x41 0x42\n")
    code, told, err = tool("build", os.path.join(work, "bdf.needs"), out)
    check(code == 1 and "a font of bitmaps, with no 12 px; its size: 20 px" in err, f"a BDF at a size it has not: exit {code}, {err.strip()}")
    alone = os.path.join(work, "bdf-alone")
    os.makedirs(alone, exist_ok=True)
    run = subprocess.run([os.path.join(out, "font2c"), os.path.join(work, "test20.bdf"), "12", "0x41", "0x42"], capture_output=True, text=True,
                         cwd=alone)
    check(run.returncode == 1 and "its size: 20 px" in run.stderr and not os.listdir(alone),
          f"font2c alone, a BDF at a size it has not: exit {run.returncode}, {run.stderr.strip()}, files {os.listdir(alone)}")


def test_json_of_a_bundled_font():
    code, text, err = tool("json", os.path.join(DGX, "src", "fonts", "TerminusTTFMedium12.c"))
    check(code == 0, f"json of a bundled font: {err.strip()}")
    if code == 0:
        font = json.loads(text)
        zero = font["glyphs"].get(str(ord("0")))
        check(font["name"] == "TerminusTTFMedium12" and zero and len(zero["rows"]) == zero["h"] and "#" in "".join(zero["rows"]),
              "the glyph of 0 has its rows of pixels")


def test_editor_round_trip(work):
    """The C of a bitmap font as the font editor reads and writes it (font-editor/FontC.js under node), against font2c,
    which writes it, and dgx_fonts.py, which reads it for editors: a font font2c makes now comes back from the editor
    byte for byte, and so does the stream the editor writes of it, which dgx_fonts.py reads as the editor does; the
    editor and dgx_fonts.py read the same glyphs from every bitmap font of DGX, and what the editor writes of one it
    reads back and writes again unchanged."""
    node = shutil.which("node")
    if not node:
        print("test_fonts.py: node is not installed, the reading and writing of the font editor is not checked")
        return

    def editor(*args):
        run = subprocess.run([node, os.path.join(HERE, "round_trip.js"), DGX] + list(args), capture_output=True, text=True)
        if run.returncode != 0:
            return {"error": run.stderr.strip()}
        return json.loads(run.stdout) if run.stdout.strip() else {}

    def glyphs_read(path):
        code, text, err = tool("json", path)
        if code != 0:
            return None
        return {cp: [g["w"], g["h"], g["xAdvance"], g["xOffset"], g["yOffset"], "|".join(g["rows"])] for cp, g in json.loads(text)["glyphs"].items()}

    made = os.path.join(work, "round-trip")
    os.makedirs(made, exist_ok=True)
    run = subprocess.run([os.path.join(work, "out", "font2c"), dejavu_file(), "14", "0x20", "0x7e", "0x410", "0x44f"], capture_output=True,
                         text=True, cwd=made)
    made_c = [f for f in os.listdir(made) if f.endswith(".c")]
    check(run.returncode == 0 and len(made_c) == 1, f"font2c makes a font for the editor: {run.stderr.strip()} {os.listdir(made)}")
    if made_c:
        path = os.path.join(made, made_c[0])
        read = editor("read", path)
        check(read.get("same"), f"the editor writes a font of font2c back byte for byte: {read.get('error', '')}")
        check(read.get("glyphs") is not None and read.get("glyphs") == glyphs_read(path), "the editor and dgx_fonts.py read a font of font2c alike")
        stream = os.path.join(made, "stream.c")
        editor("stream", path, stream)
        again = editor("read", stream) if os.path.exists(stream) else {"error": "no stream written"}
        check(again.get("same") and again.get("glyphs") == read.get("glyphs"),
              f"a stream the editor writes comes back from it byte for byte and with the same glyphs: {again.get('error', '')}")
        check(glyphs_read(stream) == read.get("glyphs"), "dgx_fonts.py reads a stream the editor writes as the editor does")
    for name in sorted(os.listdir(os.path.join(DGX, "src", "fonts"))):
        if not name.endswith(".c"):
            continue
        path = os.path.join(DGX, "src", "fonts", name)
        read = editor("read", path)
        if "no bitmap font" in read.get("error", ""):
            continue  # handwritten and dot fonts are not edited
        check("error" not in read, f"{name}: the editor reads it: {read.get('error')}")
        check(read.get("glyphs") == glyphs_read(path), f"{name}: the editor and dgx_fonts.py read the same glyphs")
        check(read.get("stable"), f"{name}: what the editor writes of it, it reads back and writes again unchanged")


def dejavu_file(name="DejaVuSans"):
    return subprocess.run(["fc-list", f":postscriptname={name}", "--format", "%{file}\n"], capture_output=True, text=True).stdout.split("\n")[0]


def glyphs_of(c_path, function):
    code, text, err = tool("json", c_path, "--font", function)
    return {chr(int(k)) for k in json.loads(text)["glyphs"]} if code == 0 else None


def test_style_order():
    """Among the faces of a family a regular one comes first, then Medium, then the rest, whatever the order of the
    files: checked on faces made up here, since the DejaVu fonts have no Medium."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("dgx_fonts", TOOL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    check(m.style_rank("Regular") < m.style_rank("Medium") < m.style_rank("Bold") and m.style_rank("Book") == m.style_rank("Regular"),
          "Regular before Medium before Bold")
    fonts = [m.Font("/x/Poppins-Medium.ttf", 0, "Poppins", "Medium", False, "Poppins-Medium", "61-7a", 0),
             m.Font("/x/Poppins-Regular.ttf", 0, "Poppins", "Regular", False, "Poppins-Regular", "61-7a", 1),
             m.Font("/x/Poppins-Bold.ttf", 0, "Poppins", "Bold", False, "Poppins-Bold", "61-7a", 2)]
    _, rules, _ = m.parse_rules("source system\nallow * 1..\n")
    found = [f.style for _, f in m.candidates(fonts, rules, 12, letters="abc")]
    check(found == ["Regular", "Medium", "Bold"], f"the order of the styles: {found}")


def test_merged_declarations(work):
    """A need declared in two places with the same keywords is one need, and its face is picked once for the characters
    of both: here the first allowed face lacks a character of the second declaration, so the second face takes both."""
    with open(os.path.join(work, "mono-first.rules"), "w", encoding="utf-8") as f:
        f.write('source system\nallow "DejaVu Sans Mono" 1..\nallow "DejaVu Sans" 1..\n')
    with open(os.path.join(work, "merged.needs"), "w", encoding="utf-8") as f:
        f.write(f"component {DGX}\nrules mono-first.rules\nneed note /a/CMakeLists.txt\nsize 20px\nletters abc\n"
                "need note /b/CMakeLists.txt\nsize 20px\nletters \u1e9e\n")
    out = os.path.join(work, "out-merged")
    code, told, err = tool("build", os.path.join(work, "merged.needs"), out)
    check(code == 0 and "note: DejaVu Sans Book 20 px" in told, f"one face for the characters of both declarations: {err.strip()} {told.strip()}")
    if code == 0:
        glyphs = glyphs_of(os.path.join(out, "dgx_fonts.c"), "dgx_font_note")
        check(glyphs is not None and {"a", "b", "c", "\u1e9e"} <= glyphs, "the characters of both declarations are in the font")


def test_rules_paths(work):
    """A relative path in the rules (source dir, source list) and in a source list is taken from the file that names it,
    not from the current directory, which is the build directory under ninja."""
    fonts = os.path.join(work, "rules", "fonts")
    os.makedirs(fonts, exist_ok=True)
    shutil.copy(dejavu_file(), os.path.join(fonts, "Sans.ttf"))
    with open(os.path.join(work, "rules", "dir.rules"), "w", encoding="utf-8") as f:
        f.write("source dir fonts\nallow * 1..\n")
    with open(os.path.join(work, "rules", "list.rules"), "w", encoding="utf-8") as f:
        f.write("source list fonts/files.txt\nallow * 1..\n")
    with open(os.path.join(fonts, "files.txt"), "w", encoding="utf-8") as f:
        f.write("Sans.ttf\n")
    elsewhere = os.path.join(work, "elsewhere")
    os.makedirs(elsewhere, exist_ok=True)
    for rules in ("dir.rules", "list.rules"):
        run = subprocess.run([sys.executable, "-I", TOOL, "pick", os.path.join(work, "rules", rules), "12"], capture_output=True, text=True, cwd=elsewhere)
        check(run.returncode == 0 and os.path.join(fonts, "Sans.ttf") in run.stdout, f"{rules}: the font of a relative source is found: {run.stderr.strip()}")


def test_half_pixels(work):
    """A size that comes to a half pixel is rounded up: 1.25 mm at 10 px/mm is 13 px, not the even 12."""
    with open(os.path.join(work, "half.needs"), "w", encoding="utf-8") as f:
        f.write(f"component {DGX}\nboard pixels 300 300\nboard mm 30 30\nneed tt\nsize 1.25mm\npostscript DejaVuSans\nletters abc\n")
    out = os.path.join(work, "out-half")
    code, told, err = tool("build", os.path.join(work, "half.needs"), out)
    check(code == 0 and "#define DGX_FONT_TT_PX 13" in open(os.path.join(out, "dgx_fonts.h"), encoding="utf-8").read(), f"12.5 px is 13: {err.strip()}")


def test_characters_as_written(work):
    """A space among the letters of a need is a character like any other, and a text with a byte order mark is read
    without it."""
    with open(os.path.join(work, "space.needs"), "w", encoding="utf-8") as f:
        f.write(f"component {DGX}\nneed tt\nsize 12px\npostscript DejaVuSans\nletters  ab\n")
    out = os.path.join(work, "out-space")
    code, told, err = tool("build", os.path.join(work, "space.needs"), out)
    glyphs = glyphs_of(os.path.join(out, "dgx_fonts.c"), "dgx_font_tt") if code == 0 else None
    check(glyphs == {" ", "a", "b"}, f"a space among the letters is kept: {err.strip()} {glyphs}")
    bom = os.path.join(work, "bom.json")
    with open(bom, "w", encoding="utf-8-sig") as f:
        f.write('{"label": "xy"}')
    with open(os.path.join(work, "bom.needs"), "w", encoding="utf-8") as f:
        f.write(f"component {DGX}\nneed tt\nsize 12px\npostscript DejaVuSans\ntext {bom}\n")
    code, told, err = tool("build", os.path.join(work, "bom.needs"), out)
    glyphs = glyphs_of(os.path.join(out, "dgx_fonts.c"), "dgx_font_tt") if code == 0 else None
    check(glyphs == {"x", "y"}, f"a JSON with a byte order mark: {err.strip()} {glyphs}")
    code, told, err = build(work, "need Title\nsize 12px\npostscript DejaVuSans\nletters ab\nneed title\nsize 12px\npostscript DejaVuSans\nletters ab\n",
                            board=False, rules=False)
    check(code == 1 and "differ only in case" in err, f"two needs that differ only in case are refused: {err.strip()}")
    # the icons of an icon font are characters of the Private Use Area: they are letters like any other, and a soft
    # hyphen or a zero-width joiner is not
    po = os.path.join(work, "escapes.po")
    with open(po, "w", encoding="utf-8") as f:
        f.write('msgid ""\nmsgstr "header"\n\nmsgid "a\\x41\\101\\\\\\"\\q"\nmsgstr "\\u"\n')
    code, out, err = tool("letters", po)
    check(code == 0 and set(out.strip("\n")) == set('aA\\"qu'), f"the escapes of a .po: {out!r} {err.strip()}")
    icons = os.path.join(work, "icons.letters")
    with open(icons, "w", encoding="utf-8") as f:
        f.write("\uf010\uf002 a\u00ad\u200d")
    code, out, err = tool("letters", icons)
    check(code == 0 and set(out.strip("\n")) == {"\uf010", "\uf002", " ", "a"}, f"private-use characters are letters, format characters are not: {out!r} {err.strip()}")
    code, told, err = build(work, f"need icons\nsize 24px\npostscript DejaVuSans\ntext {icons}\n", board=False, rules=False)
    check(code == 1 and "lacks the characters" in err and "U+F010" in err, f"a private-use character reaches the check of the face: {err.strip()}")


def test_font2c_lines(work):
    """The lines of a list through font2c itself: options after the size, a range the font has none of, two files that
    would make one font, a line without -n beside a line with -n for the same font."""
    font2c = os.path.join(work, "out", "font2c")
    sans = dejavu_file()
    with open(os.path.join(work, "after.letters"), "w", encoding="utf-8") as f:
        f.write("xyz")
    with open(os.path.join(work, "after.list"), "w", encoding="utf-8") as f:
        f.write(f"{sans} 12 -f after.letters\n")
    run = subprocess.run([font2c, "-jo", os.path.join(work, "after.list"), "-o", os.path.join(work, "after")], capture_output=True, text=True)
    glyphs = glyphs_of(os.path.join(work, "after.c"), "after_DejaVuSansBook12") if run.returncode == 0 else None
    check(glyphs == {"x", "y", "z"}, f"-f after the size is the letters file, not a range: {run.stderr.strip()} {glyphs}")
    with open(os.path.join(work, "t.list"), "w", encoding="utf-8") as f:
        f.write(f"-t 2 {sans} 12 0x30 0x39\n")
    run = subprocess.run([font2c, "-jo", os.path.join(work, "t.list"), "-o", os.path.join(work, "t")], capture_output=True, text=True)
    check(run.returncode == 1 and "-t is advice for -x" in run.stderr, f"-t in a list is refused: {run.stderr.strip()}")
    alone = os.path.join(work, "alone-run")
    os.makedirs(alone, exist_ok=True)
    run = subprocess.run([font2c, sans, "12", "0x100000", "0x100010"], capture_output=True, text=True, cwd=alone)
    check(run.returncode == 1 and "none of the characters" in run.stderr and not os.listdir(alone),
          f"a range the font has none of: exit {run.returncode}, {run.stderr.strip()}, files {os.listdir(alone)}")
    copy = os.path.join(work, "copy")
    os.makedirs(copy, exist_ok=True)
    shutil.copy(sans, os.path.join(copy, "DejaVuSans.ttf"))
    with open(os.path.join(work, "twofiles.list"), "w", encoding="utf-8") as f:
        f.write(f"{sans} 12 0x30 0x39\ncopy/DejaVuSans.ttf 12 0x41 0x5a\n")
    run = subprocess.run([font2c, "-jo", os.path.join(work, "twofiles.list"), "-o", os.path.join(work, "twofiles")], capture_output=True, text=True)
    check(run.returncode == 1 and "both make the font DejaVuSansBook12" in run.stderr, f"two files of one name are refused: {run.stderr.strip()}")
    with open(os.path.join(work, "plain.list"), "w", encoding="utf-8") as f:
        f.write(f"-n digits {sans} 24 0x30 0x39\n{sans} 24 0x41 0x5a\n")
    run = subprocess.run([font2c, "-jo", os.path.join(work, "plain.list"), "-o", os.path.join(work, "plain")], capture_output=True, text=True)
    h = open(os.path.join(work, "plain.h"), encoding="utf-8").read() if run.returncode == 0 else ""
    c = open(os.path.join(work, "plain.c"), encoding="utf-8").read() if run.returncode == 0 else ""
    check("dgx_font_t *digits(void);" in h and "dgx_font_t *plain_DejaVuSansBook24(void);" in h and "return digits();" in c,
          f"a line without -n keeps its function beside the -n name: {run.stderr.strip()}")


def test_font2c_long_paths(work):
    """A list and the output in a directory of a path over 1000 characters long: the paths of the list are taken from
    its directory, and the C and the header have their whole names."""
    font2c = os.path.join(work, "out", "font2c")
    deep = os.path.join(work, *(c * 200 for c in "lmnop"))
    os.makedirs(deep, exist_ok=True)
    shutil.copy(dejavu_file(), os.path.join(deep, "Sans.ttf"))
    with open(os.path.join(deep, "deep.list"), "w", encoding="utf-8") as f:
        f.write("-n deep_font Sans.ttf 12 0x41 0x42\n")
    run = subprocess.run([font2c, "-jo", os.path.join(deep, "deep.list"), "-o", os.path.join(deep, "deep")], capture_output=True, text=True)
    made = sorted(os.listdir(deep))
    check(run.returncode == 0 and made == ["Sans.ttf", "deep.c", "deep.h", "deep.list"] and
          glyphs_of(os.path.join(deep, "deep.c"), "deep_font") == {"A", "B"}, f"a list and an output deep down: {run.stderr.strip()} {made}")


def test_patches(work):
    """The fixes of the font editor: a patch found by the name of the font beside the needs, one named by a need, one
    of another size refused, a glyph of the patch of another size than the rendered one, rows of unequal length, a
    patch of several fonts found by what it holds."""
    pd = os.path.join(work, "patched")
    os.makedirs(pd, exist_ok=True)
    a_rows = ["#..#", ".##.", "#..#"]

    def patch(path, glyphs, version=2, font="DejaVuSansBook12", size=12):
        body = {"format": "dgx-font-patch", "formatVersion": version, "glyphs": glyphs}
        if version == 2:
            body.update({"font": font, "size": size})
        with open(path, "w", encoding="utf-8") as f:
            json.dump(body, f)

    patch(os.path.join(pd, "DejaVuSansBook12.patch.json"), {"0041": {"rows": a_rows, "xAdvance": 9, "xOffset": 1, "yOffset": -5}})
    with open(os.path.join(pd, "p.needs"), "w", encoding="utf-8") as f:
        f.write("need pat\nsize 12px\npostscript DejaVuSans\nletters AB\n")
    out = os.path.join(pd, "out")
    code, told, err = tool("build", os.path.join(pd, "p.needs"), out)
    check(code == 0 and "patched by" in told, f"a patch beside the needs, found by the name of the font: {err.strip()} {told.strip()}")
    if code == 0:
        code, text, err = tool("json", os.path.join(out, "dgx_fonts.c"), "--font", "dgx_font_pat")
        glyphs = json.loads(text)["glyphs"] if code == 0 else {}
        a = glyphs.get(str(ord("A")), {})
        check(a.get("rows") == a_rows and (a.get("w"), a.get("h"), a.get("xAdvance"), a.get("xOffset"), a.get("yOffset")) == (4, 3, 9, 1, -5),
              f"the glyph of the patch stands whole, of its own size and metrics: {a}")
        check(str(ord("B")) in glyphs and glyphs[str(ord("B"))]["rows"] != a_rows, "a glyph the patch has not is rendered")
    # a patch named by the need, of version 1, from another directory
    other = os.path.join(work, "other.patch.json")
    patch(other, {"0042": {"rows": ["##", "##"], "xAdvance": 3, "xOffset": 0, "yOffset": -2}}, version=1)
    with open(os.path.join(pd, "q.needs"), "w", encoding="utf-8") as f:
        f.write(f"need pat\nsize 12px\npostscript DejaVuSans\nletters AB\npatch {other}\n")
    code, told, err = tool("build", os.path.join(pd, "q.needs"), out)
    if code == 0:
        code, text, err = tool("json", os.path.join(out, "dgx_fonts.c"), "--font", "dgx_font_pat")
        glyphs = json.loads(text)["glyphs"] if code == 0 else {}
        check(glyphs.get(str(ord("B")), {}).get("rows") == ["##", "##"] and glyphs.get(str(ord("A")), {}).get("rows") != a_rows,
              "the patch the need names wins over the one found by name")
    else:
        check(False, f"a patch named by the need: {err.strip()}")
    # a patch of another size, named
    wrong = os.path.join(work, "wrong.patch.json")
    patch(wrong, {"0041": {"rows": a_rows}}, size=14)
    with open(os.path.join(pd, "r.needs"), "w", encoding="utf-8") as f:
        f.write(f"need pat\nsize 12px\npostscript DejaVuSans\nletters AB\npatch {wrong}\n")
    code, told, err = tool("build", os.path.join(pd, "r.needs"), out)
    check(code == 1 and "the patch is for the font DejaVuSansBook12 at 14 px" in err, f"a patch of another size is refused: {err.strip()}")
    # rows of unequal length
    patch(wrong, {"0041": {"rows": ["##", "###"]}})
    code, told, err = tool("build", os.path.join(pd, "r.needs"), out)
    check(code == 1 and "U+0041 are not all as long" in err, f"rows of unequal length are refused: {err.strip()}")
    # font2c alone: -P, a glyph of the patch the face has not, a handwritten font with -p
    font2c = os.path.join(out, "font2c")
    alone = os.path.join(work, "patch-alone")
    os.makedirs(alone, exist_ok=True)
    patch(os.path.join(pd, "DejaVuSansBook12.patch.json"), {"0041": {"rows": a_rows}, "10FFFD": {"rows": ["#"]}})
    run = subprocess.run([font2c, "-P", pd, dejavu_file(), "12", "0x41", "0x42"], capture_output=True, text=True, cwd=alone)
    check(run.returncode == 0 and "U+10FFFD of the patch is not in the face" in run.stderr and "patched by" in run.stdout,
          f"-P finds the patch, a glyph the face has not is told: {run.stderr.strip()}")
    run = subprocess.run([font2c, "-p", other, "font.json"], capture_output=True, text=True, cwd=alone)
    check(run.returncode == 1 and "a handwritten font is fixed in its editor" in run.stderr, f"-p with a handwritten font: {run.stderr.strip()}")
    # dgx_fonts.py reads a list with -p and -P as font2c does
    with open(os.path.join(pd, "p.list"), "w", encoding="utf-8") as f:
        f.write(f"-n listed -P . -p {other} {dejavu_file()} 12 0x41 0x42\n")
    with open(os.path.join(pd, "l.needs"), "w", encoding="utf-8") as f:
        f.write(f"list {os.path.join(pd, 'p.list')}\n")
    code, told, err = tool("build", os.path.join(pd, "l.needs"), out)
    check(code == 0 and "listed: from a list, 2 glyphs" in told, f"a list with -p and -P: {err.strip()} {told.strip()}")
    # a patch of several fonts, version 3, under a name of its own: every font finds its part in it
    md = os.path.join(work, "multi")
    os.makedirs(md, exist_ok=True)
    several = os.path.join(md, "fonts.patch.json")
    with open(several, "w", encoding="utf-8") as f:
        json.dump({"format": "dgx-font-patch", "formatVersion": 3, "fonts": [
            {"font": "DejaVuSansBook12", "size": 12, "glyphs": {"0041": {"rows": a_rows, "xAdvance": 9, "xOffset": 1, "yOffset": -5}}},
            {"font": "DejaVuSansBook14", "size": 14, "glyphs": {"0042": {"rows": ["##", "##"], "xAdvance": 3, "xOffset": 0, "yOffset": -2}}},
            {"font": "DejaVuSansBook30", "size": 30, "glyphs": {}}]}, f)
    with open(os.path.join(md, "m.needs"), "w", encoding="utf-8") as f:
        f.write("need small\nsize 12px\npostscript DejaVuSans\nletters AB\n"
                "need large\nsize 14px\npostscript DejaVuSans\nletters AB\n")
    mout = os.path.join(md, "out")
    code, told, err = tool("build", os.path.join(md, "m.needs"), mout)
    check(code == 0 and told.count(f"patched by {several}") == 2, f"a patch of several fonts beside the needs patches each: {err.strip()} {told.strip()}")
    if code == 0:
        small = json.loads(tool("json", os.path.join(mout, "dgx_fonts.c"), "--font", "dgx_font_small")[1])["glyphs"]
        large = json.loads(tool("json", os.path.join(mout, "dgx_fonts.c"), "--font", "dgx_font_large")[1])["glyphs"]
        check(small[str(ord("A"))]["rows"] == a_rows and small[str(ord("B"))]["rows"] != ["##", "##"] and
              large[str(ord("B"))]["rows"] == ["##", "##"] and large[str(ord("A"))]["rows"] != a_rows,
              "every font takes its own part of the patch, and only that")
    # two patches of one directory for one font
    shutil.copy(several, os.path.join(md, "again.patch.json"))
    code, told, err = tool("build", os.path.join(md, "m.needs"), mout)
    check(code == 1 and "one font comes from one patch" in err, f"two patches for one font beside the needs: {err.strip()}")
    os.remove(os.path.join(md, "again.patch.json"))
    # named by a need, but with no part for its font
    with open(os.path.join(md, "n.needs"), "w", encoding="utf-8") as f:
        f.write(f"need mid\nsize 13px\npostscript DejaVuSans\nletters AB\npatch {several}\n")
    code, told, err = tool("build", os.path.join(md, "n.needs"), mout)
    check(code == 1 and "the patch has no part for the font DejaVuSansBook13 at 13 px; its fonts: DejaVuSansBook12 at 12 px" in err,
          f"a patch of several fonts named for a font it has no part for: {err.strip()}")
    # the patches that come with the fonts of DGX: Droid Sans at 9 px made at build time takes src/fonts/DroidSansRegular9.patch.json
    droid = next((p for p in (os.path.expanduser("~/.wine/drive_c/windows/Fonts/droidsans.ttf"), "/usr/share/fonts/truetype/droid/DroidSans.ttf")
                  if os.path.exists(p)), None)
    if droid:
        with open(os.path.join(pd, "d.needs"), "w", encoding="utf-8") as f:
            f.write(f"need droid\nsize 9px\nsource {droid}\nletters %&A\n")
        code, told, err = tool("build", os.path.join(pd, "d.needs"), out)
        check(code == 0 and os.path.join(DGX, "src", "fonts", "DroidSansRegular9.patch.json") in told,
              f"a font made at build time takes the patch that comes with the font of DGX of the same name: {err.strip()} {told.strip()}")
    else:
        print("test_patches: no Droid Sans file here, the patch of a font of DGX is not tried")


def test_cmake(work):
    """project_include.cmake on the host, with the two ESP-IDF functions it uses as stubs: a configure and a build with
    needs from a CMakeLists, a needs file and a list, a list edited and a face the rules found changed, and the
    mistakes a CMakeLists can make."""
    cm = os.path.join(work, "cmake")
    os.makedirs(cm, exist_ok=True)
    sans = dejavu_file()
    with open(os.path.join(cm, "stub.c"), "w") as f:
        f.write("int dgx_stub;\n")
    with open(os.path.join(cm, "t.txt"), "w", encoding="utf-8") as f:
        f.write("tq")
    with open(os.path.join(cm, "fonts.rules"), "w", encoding="utf-8") as f:
        f.write('source system\nallow "DejaVu Sans" 1..\n')
    with open(os.path.join(cm, "x.needs"), "w", encoding="utf-8-sig") as f:
        f.write("need fromfile\nsize 12px\npostscript DejaVuSans\nletters [{]; \ntext t.txt\n")
    with open(os.path.join(cm, "own.letters"), "w", encoding="utf-8") as f:
        f.write("xyz")
    with open(os.path.join(cm, "own.list"), "w", encoding="utf-8") as f:
        f.write(f"-n widget {sans} 12 -f own.letters\n")
    # the fixes of the font editor beside the CMakeLists, found by the name of the font
    with open(os.path.join(cm, "DejaVuSansBook12.patch.json"), "w", encoding="utf-8") as f:
        json.dump({"format": "dgx-font-patch", "formatVersion": 2, "font": "DejaVuSansBook12", "size": 12,
                   "glyphs": {"0074": {"rows": ["#.#", ".#.", "#.#"], "xAdvance": 5, "xOffset": 1, "yOffset": -3}}}, f)
    # rules that take their faces from a list of files, a copy of DejaVu Sans here: what the build picks by them
    os.makedirs(os.path.join(cm, "faces"), exist_ok=True)
    shutil.copy(sans, os.path.join(cm, "faces", "Sans.ttf"))
    with open(os.path.join(cm, "faces.txt"), "w", encoding="utf-8") as f:
        f.write("faces/Sans.ttf\n")
    with open(os.path.join(cm, "faces.rules"), "w", encoding="utf-8") as f:
        f.write('source list faces.txt\nallow "DejaVu Sans" 1..\n')
    with open(os.path.join(cm, "CMakeLists.txt"), "w", encoding="utf-8") as f:
        f.write(f"""cmake_minimum_required(VERSION 3.19)
project(dgx_fonts_on_the_host C)
set(COMPONENT_DIR "{DGX}")
set(COMPONENT_NAME dgx)
function(idf_build_get_property out name)
    set(${{out}} "{sys.executable}" PARENT_SCOPE)
endfunction()
function(idf_component_get_property out component property)
    set(${{out}} dgx PARENT_SCOPE)
endfunction()
add_library(dgx STATIC stub.c)
target_include_directories(dgx PRIVATE "{DGX}/include" "{STUBS}")
target_compile_options(dgx PRIVATE -include "{STUBS}/esp_log.h")
include("{DGX}/project_include.cmake")
dgx_fonts_board(PIXELS 320 240 MM 57.6 43.2)
if(CASE STREQUAL "faces")
    dgx_fonts_rules(faces.rules)
else()
    dgx_fonts_rules(fonts.rules)
endif()
if(CASE STREQUAL "good")
    dgx_font_need(brackets SIZE 12px LETTERS "[a;b]" TEXTS t.txt)
    dgx_font_needs_file(x.needs)
    dgx_font_list(own.list)
elseif(CASE STREQUAL "faces")
    dgx_font_need(picked SIZE 12px TEXTS t.txt)
elseif(CASE STREQUAL "missing")
    dgx_font_need(b SIZE 12px POSTSCRIPT DejaVuSans TEXTS none.txt)
elseif(CASE STREQUAL "twice")
    dgx_font_need(b SIZE 12px SIZE 14px POSTSCRIPT DejaVuSans TEXTS t.txt)
endif()
""")

    def configure(case):
        build_dir = os.path.join(cm, f"build-{case}")
        run = subprocess.run(["cmake", "-S", cm, "-B", build_dir, "-G", "Ninja", f"-DCASE={case}"], capture_output=True, text=True)
        return build_dir, run

    build_dir, run = configure("good")
    check(run.returncode == 0, f"configure with needs: {run.stderr.strip()[-600:]}")
    if run.returncode == 0:
        needs = open(os.path.join(build_dir, "dgx_fonts", "needs.txt"), encoding="utf-8").read()
        check("letters [a;b]\n" in needs, "brackets and a semicolon of LETTERS reach the needs file")
        check("letters [{]; \n" in needs, "the letters of a needs file reach it as written, the space at the end too")
        check(f"text {os.path.join(cm, 't.txt')}" in needs and "need fromfile" in needs, "a needs file with a byte order mark is read")
        ninja = open(os.path.join(build_dir, "build.ninja"), encoding="utf-8").read()
        check("own.letters" in ninja, "the file of -f in a list is an input of the rule")
        check("DejaVuSansBook12.patch.json" in ninja, "a patch beside the CMakeLists is an input of the rule")
        run = subprocess.run(["cmake", "--build", build_dir], capture_output=True, text=True)
        check(run.returncode == 0, f"build: {(run.stdout + run.stderr).strip()[-800:]}")
        if run.returncode == 0:
            h = open(os.path.join(build_dir, "dgx_fonts", "dgx_fonts.h"), encoding="utf-8").read()
            check("dgx_font_t *dgx_font_brackets(void);" in h and "dgx_font_t *dgx_font_fromfile(void);" in h and "dgx_font_t *widget(void);" in h,
                  "the three fonts of the three ways are in the header")
            c = os.path.join(build_dir, "dgx_fonts", "dgx_fonts.c")
            # the three are one face at 12 px, so one font with the characters of all three
            check(set("[a;b]tq") <= (glyphs_of(c, "dgx_font_brackets") or set()), "the characters of LETTERS with brackets and a semicolon are in the font")
            check(set("[{]; tqxyz") <= (glyphs_of(c, "dgx_font_fromfile") or set()), "the characters of the needs file, the space too, are in the font")
            check("widget: from a list, 12 glyphs" in run.stdout, f"a list's font that shares a need's font is counted: {run.stdout.strip()[-200:]}")
            check("patched by" in run.stdout, f"the font of the needs takes the patch beside the CMakeLists: {run.stdout.strip()[-300:]}")
            # a file a list names anew is an input of the rule: the list configures the project again
            with open(os.path.join(cm, "more.letters"), "w", encoding="utf-8") as f:
                f.write("w")
            with open(os.path.join(cm, "own.list"), "a", encoding="utf-8") as f:
                f.write(f"-n widget2 {sans} 12 -f more.letters\n")
            run = subprocess.run(["cmake", "--build", build_dir], capture_output=True, text=True)
            ninja = open(os.path.join(build_dir, "build.ninja"), encoding="utf-8").read()
            check(run.returncode == 0 and "more.letters" in ninja, f"a list edited configures the project again: {run.stdout.strip()[-300:]}")
    # the face the rules found is a dependency of the rule, by the depfile the script writes
    build_dir, run = configure("faces")
    check(run.returncode == 0, f"configure with rules of a list of faces: {run.stderr.strip()[-600:]}")
    if run.returncode == 0:
        run = subprocess.run(["cmake", "--build", build_dir], capture_output=True, text=True)
        depfile = os.path.join(build_dir, "dgx_fonts", "dgx_fonts.d")
        named = open(depfile, encoding="utf-8").read() if os.path.exists(depfile) else ""
        check(run.returncode == 0 and os.path.join(cm, "faces", "Sans.ttf") in named and os.path.join(cm, "faces.txt") in named,
              f"the depfile names the face the rules picked and their list of faces: {run.stdout.strip()[-300:]} {named}")
        run = subprocess.run(["cmake", "--build", build_dir], capture_output=True, text=True)
        check("no work to do" in run.stdout, f"nothing changed, nothing made: {run.stdout.strip()[-300:]}")
        os.utime(os.path.join(cm, "faces", "Sans.ttf"))
        run = subprocess.run(["cmake", "--build", build_dir], capture_output=True, text=True)
        check("DGX: the fonts of the program by its needs" in run.stdout, f"the face changed, the fonts made again: {run.stdout.strip()[-300:]}")
    for case, expected in (("missing", "none.txt does not exist"), ("twice", "SIZE given twice")):
        build_dir, run = configure(case)
        said = " ".join(run.stderr.split())  # CMake wraps its messages
        check(run.returncode != 0 and expected in said, f"{case}: told '{expected}': {said[-300:]}")


def test_licenses(work):
    """The fonts a program uses are listed with what their files say of their licenses; a deny of the program's rules
    holds for every way a font is asked for, and strict rules admit only what their allow lines name."""
    title = os.path.join(HERE, "title.txt")
    rules = os.path.join(work, "lic.rules")
    terminus = os.path.join(DGX, "src", "fonts", "TerminusTTFMedium12.c")
    with open(os.path.join(work, "lic.list"), "w", encoding="utf-8") as f:
        f.write(f"-n widget {dejavu_file('DejaVuSansMono')} 12 0x41 0x5a\n")

    def run(rules_text, needs, lists=""):
        with open(rules, "w", encoding="utf-8") as f:
            f.write(rules_text)
        path = os.path.join(work, "lic.needs")
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"component {DGX}\nscale 4\nboard pixels 1280 800\nboard mm 217 136\nrules {rules}\n{lists}{needs}")
        shutil.rmtree(os.path.join(work, "lic"), ignore_errors=True)
        return tool("build", path, os.path.join(work, "lic"))

    exact = f"need keys /k/CMakeLists.txt\nsize 4mm\npostscript DejaVuSans\ntext {title}\n"
    as_is = f"need clock /c/CMakeLists.txt\ndgx TerminusTTFMedium12\nletters 0123456789:\n"
    program = f"need time /t/CMakeLists.txt\nfile {terminus}\nletters 0123456789:\n"
    picked = f"need text /p/CMakeLists.txt\nsize 12pt\nmono 1\ntext {title}\n"
    listed = f"list {os.path.join(work, 'lic.list')}\n"
    # the report: every font once, with its license, who uses it and how it came
    code, out, err = run("source system\nallow \"DejaVu Sans Mono\" 8..\n", exact + as_is + picked, listed)
    check(code == 0, f"a build with every kind of font: {err.strip()}")
    used = os.path.join(work, "lic", "fonts-used.txt")
    check("fonts-used.txt; make sure you may use every one of them" in out, f"the build tells where the list of fonts is: {out.strip()}")
    if code == 0:
        text = open(used, encoding="utf-8").read()
        check("\nDejaVu Sans Book\n" in text and "  used by: keys at 24 px, named by its PostScript name in /k/CMakeLists.txt" in text,
              "a face named exactly is listed with who named it")
        check("  used by: text at 48 px, picked by the rules, " in text and "  used by: a list at 12 px, " in text,
              "a face picked by the rules and a face of a list are listed once, with both uses")
        check(text.count("\nDejaVu Sans Mono Book\n") == 1, "one face, one entry")
        check("license URL: " in text, "what the font file says of its license is in the list")
        check("TerminusTTFMedium12, a font of DGX\n" in text and "made from: Terminus (TTF) Medium" in text and "SIL Open Font License 1.1" in text,
              "a font of DGX is listed with its origin from src/fonts/README.md")
        h = open(os.path.join(work, "lic", "dgx_fonts.h"), encoding="utf-8").read()
        check("fonts-used.txt beside this file" in h, "the header points at the list of fonts")
    # a deny holds for every way a font is asked for, and the message names both sides
    cases = [
        ("a face named exactly", 'deny "DejaVu Sans"\n', exact,
         "/k/CMakeLists.txt: keys asks for DejaVu Sans Book"),
        ("a font of DGX by its family", 'deny "Terminus*"\n', as_is, "/c/CMakeLists.txt: clock asks for TerminusTTFMedium12 of DGX"),
        ("a font of the program by its name", 'deny "TerminusTTF*"\n', program, "/t/CMakeLists.txt: time asks for TerminusTTFMedium12 of"),
        ("a font of the program by the family as written", 'deny "Terminus (TTF)*"\n', program, "/t/CMakeLists.txt: time asks for"),
        ("a font of a list", 'deny "DejaVu Sans Mono"\n', listed, "lic.list:1: the list asks for DejaVu Sans Mono Book"),
    ]
    for what, deny, needs, expected in cases:
        code, out, err = run(deny, needs if not needs.startswith("list") else exact.replace("DejaVuSans", "DejaVuSans-Bold"),
                             needs if needs.startswith("list") else "")
        check(code == 1 and expected in err and "lic.rules:1 denies it" in err, f"denied, {what}: got exit {code}: {err.strip()}")
    code, out, err = run('deny "DejaVu Sans" < 20\n', exact)
    check(code == 0, f"a deny with sizes leaves a face named exactly at another size: {err.strip()}")
    code, out, err = run('deny "Terminus*" < 20\n', as_is)
    check(code == 0, f"a deny with sizes leaves a font used as is, which has no size: {err.strip()}")
    # strict: only what an allow line names, however asked for
    code, out, err = run('strict\nallow "DejaVu Sans Mono" 8..\n', exact)
    check(code == 1 and "are strict and allow no such font" in err, f"strict rules refuse a face no allow line names: {err.strip()}")
    code, out, err = run('strict\nallow "DejaVu Sans" 100..\nallow "Terminus*" 100..\n', exact + as_is)
    check(code == 0, f"strict rules admit a font an allow line names, at any size: {err.strip()}")
    code, out, err = run('strict extra\n', exact)
    check(code == 1 and "strict takes nothing after it" in err, f"strict with words after it: {err.strip()}")
    # rules without sources: enough for denying, not for picking
    code, out, err = run('deny "Comic*"\n', picked)
    check(code == 1 and "no source of fonts" in err, f"rules that pick need a source: {err.strip()}")
    # every font of DGX has its row in src/fonts/README.md
    import importlib.util
    spec = importlib.util.spec_from_file_location("dgx_fonts", TOOL)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    origins = m.dgx_origins(DGX, {})
    fonts = sorted(os.path.splitext(f)[0] for f in os.listdir(os.path.join(DGX, "src", "fonts")) if f.endswith(".c"))
    check(sorted(origins) == fonts, f"src/fonts/README.md has a row for every font of DGX and no other: {sorted(set(fonts) ^ set(origins))}")


def main():
    if not have_dejavu():
        print("test_fonts.py: DejaVu Sans and DejaVu Sans Mono are not installed (fontconfig), nothing checked")
        return 0
    work = tempfile.mkdtemp(prefix="dgx-fonts-")
    try:
        test_build(work)
        test_mistakes(work)
        test_needs_file(work)
        test_twice(work)
        test_lists(work)
        test_bitmap_font_in_a_list(work)
        test_json_of_a_bundled_font()
        test_style_order()
        test_merged_declarations(work)
        test_rules_paths(work)
        test_half_pixels(work)
        test_characters_as_written(work)
        test_font2c_lines(work)
        test_font2c_long_paths(work)
        test_editor_round_trip(work)
        test_patches(work)
        test_cmake(work)
        test_licenses(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"test_fonts.py: {failures} failure(s)")
        return 1
    print("test_fonts.py: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
