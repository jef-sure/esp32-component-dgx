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
STUBS = os.path.join(DGX, "test", "host", "stubs")
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


def test_json_of_a_bundled_font():
    code, text, err = tool("json", os.path.join(DGX, "src", "fonts", "TerminusTTFMedium12.c"))
    check(code == 0, f"json of a bundled font: {err.strip()}")
    if code == 0:
        font = json.loads(text)
        zero = font["glyphs"].get(str(ord("0")))
        check(font["name"] == "TerminusTTFMedium12" and zero and len(zero["rows"]) == zero["h"] and "#" in "".join(zero["rows"]),
              "the glyph of 0 has its rows of pixels")


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
        test_json_of_a_bundled_font()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        print(f"test_fonts.py: {failures} failure(s)")
        return 1
    print("test_fonts.py: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
