#!/usr/bin/env python3
"""
The fonts of a DGX program, generated once for every need in the project.

A need is a font that a component or the program asks for: a name, the characters the font must contain, and
either a face and a size to render with font2c, or an existing font. Needs are declared in CMake with
dgx_font_need() from DGX's project_include.cmake; at the end of the configure step they arrive here in one file:

    dgx_fonts.py build NEEDS OUTDIR [--scale N] [--pixels W H] [--mm W H] [--rules FILE] [--component DIR]
        NEEDS is the file CMake wrote, or one written by hand in the same format (below): a program that is also
        built on the host, as a game engine is, keeps its needs in such a file, declares
        dgx_font_needs_file(fonts.needs) in its CMakeLists, and the host build runs this command with the board
        given as options. OUTDIR receives dgx_fonts.c and dgx_fonts.h, rewritten only when they change, font2c
        built from its source, and font2c's working files
    dgx_fonts.py pick RULES SIZE [--mono] [--family PATTERN] [--letters TEXT]
    dgx_fonts.py list RULES SIZE [--mono] [--family PATTERN] [--letters TEXT]
        the face the rules pick for SIZE pixels, or every face they allow, in order of preference
    dgx_fonts.py letters FILE...
        the characters found in the texts, on one line
    dgx_fonts.py json FONT.c [OUT.json] [--font NAME]
        a DGX bitmap font as JSON for editors: a .c written by font2c, or one font of dgx_fonts.c by function name

The needs file (# starts a comment; relative paths are resolved from the file's directory; a keyword given twice
is an error):

    component /path/to/the/DGX/component where font2c and the fonts of DGX are; by default, where this script is
    scale 4                              display pixels per point of the frame (for sizes in pt); 1 by default
    board pixels 1280 800                the display (for sizes in mm)
    board mm 217 136
    rules /path/fonts.rules              how faces are picked for needs declared by family: the rules, see below
    need text /abs/main/CMakeLists.txt   a need; its keywords follow, up to the next need; after the name, where
                                         it was declared (CMake writes it), for the message when a name is
                                         declared twice
    size 12pt                            12pt: points of the frame, times the scale; 4.5mm; 30px
    mono 1                               a monospaced face only
    family DejaVu Sans*                  this family only (a pattern, as in the rules)
    postscript DejaVuSans                exactly this face, found by its PostScript name; no rules needed
    text /abs/ru.po                      files whose characters the font must contain: the strings of a .po or
                                         .pot, the string values of a JSON, any other file as a whole
    letters 0123456789:                  characters given directly
    except {}                            characters of the texts the font need not contain: the braces of
                                         placeholders, for example
    dgx font0ss                          one of DGX's own fonts, used as is
    file /abs/main/fonts/PxCgaThin8.c    a font of the program, used as is (the program compiles it)
    source /abs/font.ttf                 a font file to render at the size; or a handwritten font's JSON, which
                                         has no size
    list /abs/fonts.list                 a list of fonts for font2c -jo, passed through: its fonts go into the
                                         same output file

A font gets only the characters that were asked for: those of its texts and its `letters`. The needs are passed to
font2c as one list (-jo), one line per need, with -n dgx_font_<need> and a file of its characters: font2c renders
one font per face and size with the characters of every need that uses it, and every need gets its own function,
dgx_font_<need>(). An existing font must contain the characters of its need, or the build stops; a space is never
required of it, every DGX font handles spaces. A need declared twice, by two components say, is one need when both
resolve to the same font, with the characters of both; two different fonts under one name stop the build, and the
message names both places.

The rules, one per line (# starts a comment), the same as the rules of DGX-GE were:

    source list ~/fonts.txt        a file naming font files, one path per line
    source dir  ~/Fonts            every .ttf, .otf, .ttc under it, recursively
    source system                  the installed fonts (fontconfig)
    allow "Terminus*"  > 12 & < 40 a family (or "family style"; * matches any run of characters; case is ignored)
                                   at these sizes
    allow "DejaVu Sans Mono" 16..  sizes: 14 | 16..24 | 16.. | ..24 | > 12 | >= 12 | < 40 | <= 40; & means and,
    deny  "Comic*"                 a comma or a space between them means or; deny without sizes: at every size
    allow * 40..
    strict                         only the fonts the allow lines name, however they are asked for (off by default)

The allow lines are in order of preference: the first one with a face of the size that has all the characters wins;
among its faces a regular style comes first, then Medium, then bold, italic and the rest, then the order of the
sources. A face denied at the size is never taken. A face is found by its PostScript name with fc-list and must
match exactly: fc-match would silently return another face for a name it does not know.

The fonts and their licenses: each build writes fonts-used.txt beside dgx_fonts.c, every font the program uses with
what its file says of its copyright and license and who asks for it. Whether the program may use them is for its
author to judge; the rules are where they say no. A deny holds for every font, not only for those the rules pick: a
need or a list that names a denied font exactly (postscript, source, dgx, file, a list) stops the build, naming both
sides; no other face is put in its place. A font used as is has no size, so only a deny without sizes concerns it,
and its name is matched without spaces and punctuation (deny "DejaVu Sans*" matches DejaVuSansBook32), a font of DGX by its
family too, as src/fonts/README.md records it. With strict, a font gets in only when an allow line names it; the
sizes of the allow lines still choose among faces, but do not keep out a font named exactly. Rules with no
source lines are enough for denying; picking a face needs a source.
"""
import argparse
import fnmatch
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import unicodedata
from collections import namedtuple

Font = namedtuple("Font", "file index family style mono psname charset order")
Rule = namedtuple("Rule", "allow pattern sizes where")
Rules = namedtuple("Rules", "path sources rules strict")  # a rules file: strict when only its allow lines admit a font


class FontsError(Exception):
    pass


FONT_FILES = (".ttf", ".otf", ".ttc")
PLAIN = ("regular", "book", "normal", "roman", "plain", "")


def style_rank(style):
    """The order of the styles of one family: a regular face first, then Medium, then bold, italic and the rest."""
    s = style.lower()
    return 0 if s in PLAIN else 1 if s == "medium" else 2
FC_FORMAT = "%{file}|%{index}|%{family[0]}|%{style[0]}|%{spacing}|%{postscriptname}|%{charset}\\n"


# ----------------------------------------------------------------------------------------------------------------
#  The rules and the faces
# ----------------------------------------------------------------------------------------------------------------

def parse_sizes(text, where):
    """Sizes as alternatives, each a list of conditions (op, n) that hold together: [[(">", 12), ("<", 40)], ...]."""
    tokens = re.findall(r">=|<=|>|<|&|,|\.\.|\d+|\S", text)
    alternatives, i = [], 0

    def number():
        nonlocal i
        if i < len(tokens) and tokens[i].isdigit():
            i += 1
            return int(tokens[i - 1])
        raise FontsError(f"{where}: a number expected in sizes '{text}'")

    def condition():
        nonlocal i
        tok = tokens[i] if i < len(tokens) else None
        if tok in (">", ">=", "<", "<="):
            i += 1
            return [(tok, number())]
        if tok == "..":
            i += 1
            return [("<=", number())]
        low = number()
        if i < len(tokens) and tokens[i] == "..":
            i += 1
            if i < len(tokens) and tokens[i].isdigit():
                return [(">=", low), ("<=", number())]
            return [(">=", low)]
        return [("==", low)]

    while i < len(tokens):
        if tokens[i] == ",":
            i += 1
            continue
        conds = condition()
        while i < len(tokens) and tokens[i] == "&":
            i += 1
            if i >= len(tokens):
                raise FontsError(f"{where}: nothing after & in sizes '{text}'")
            conds += condition()
        alternatives.append(conds)
    return alternatives


def size_allowed(sizes, size):
    """Whether size is one of the sizes (None: all of them)."""
    if sizes is None:
        return True
    ops = {"==": lambda a, b: a == b, ">": lambda a, b: a > b, ">=": lambda a, b: a >= b, "<": lambda a, b: a < b, "<=": lambda a, b: a <= b}
    return any(all(ops[op](size, n) for op, n in conds) for conds in sizes)


def parse_rules(text, name="rules", base=None):
    """The sources [(kind, path or None)], the rules [Rule] and whether they are strict, of a text of rules; a relative
    path of a source is resolved from `base`, the directory of the rules file."""
    sources, rules, strict = [], [], False
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        where = f"{name}:{number}"
        word, _, rest = line.partition(" ")
        rest = rest.strip()
        if word == "source":
            kind, _, path = rest.partition(" ")
            path = path.strip()
            if kind == "system" and not path:
                sources.append(("system", None))
            elif kind in ("list", "dir") and path:
                path = os.path.expanduser(path)
                sources.append((kind, os.path.normpath(os.path.join(base, path)) if base and not os.path.isabs(path) else path))
            else:
                raise FontsError(f"{where}: source list <file>, source dir <dir> or source system")
        elif word in ("allow", "deny"):
            m = re.fullmatch(r'"([^"]+)"\s*(.*)|(\*)\s*(.*)', rest)
            if not m:
                raise FontsError(f'{where}: {word} "<family>" <sizes> (or * for any family)')
            pattern, sizes = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
            if word == "allow" and not sizes.strip():
                raise FontsError(f"{where}: allow without sizes: give them (16.. means 16 and up)")
            rules.append(Rule(word == "allow", pattern.lower(), parse_sizes(sizes, where) if sizes.strip() else None, where))
        elif word == "strict":
            if rest:
                raise FontsError(f"{where}: strict takes nothing after it")
            strict = True
        else:
            raise FontsError(f"{where}: source, allow, deny or strict, not '{word}'")
    return sources, rules, strict


def matches(pattern, font):
    """The pattern of a rule against the family, or the family and the style."""
    return fnmatch.fnmatchcase(font.family.lower(), pattern) or fnmatch.fnmatchcase(f"{font.family} {font.style}".lower(), pattern)


def matches_name(pattern, name):
    """The pattern of a rule against a name of a font used as is, both without the spaces and the punctuation that
    font2c leaves out of the names it gives: "Terminus (TTF)*" and "DejaVu Sans*" match TerminusTTFMedium12 and
    DejaVuSansBook32."""
    def bare(text):
        return re.sub(r"[^0-9a-z*?\[\]!]", "", text.lower())
    return fnmatch.fnmatchcase(bare(name), bare(pattern))


def read_rules(path):
    """The program's rules file as Rules."""
    try:
        text = open(path, encoding="utf-8-sig").read()
    except OSError as e:
        raise FontsError(f"rules {path}: {e.strerror}")
    return Rules(path, *parse_rules(text, path, os.path.dirname(os.path.abspath(path))))


def admit(rules, asked, px=None, face=None, names=()):
    """Stops the build when a font that a need or a list names exactly is kept out by the program's rules: denied by
    them, or, when they are strict, taken by none of their allow lines. A face to render is judged as the rules judge
    faces, and a deny with sizes keeps it out at those sizes only; a font used as is is judged by its names, and no
    sizes concern it. `asked` says who asks for the font and how."""
    if rules is None:
        return

    def hit(rule):
        return matches(rule.pattern, face) if face else any(matches_name(rule.pattern, n) for n in names)

    for rule in rules.rules:
        if not rule.allow and hit(rule) and (rule.sizes is None or px is not None and size_allowed(rule.sizes, px)):
            raise FontsError(f"{asked}, and {rule.where} denies it: the request and the rules disagree, one of them must change")
    if rules.strict and not any(rule.allow and hit(rule) for rule in rules.rules):
        raise FontsError(f"{asked}, and the rules {rules.path} are strict and allow no such font: allow it there, or ask for another")


_charsets = {}


def charset(font):
    """The code points a face has, by fontconfig."""
    if font.charset not in _charsets:
        points = set()
        for token in font.charset.split():
            low, _, high = token.partition("-")
            try:
                a, b = int(low, 16), int(high, 16) if high else int(low, 16)
            except ValueError:
                continue
            points.update(range(a, b + 1))
        _charsets[font.charset] = points
    return _charsets[font.charset]


def missing_letters(points, letters):
    """The letters not among the code points, in order."""
    return "".join(c for c in letters if ord(c) not in points)


def candidates(fonts, rules, size, mono=False, family=None, letters=""):
    """Every face the rules allow at size that has the letters, in the order they would be taken: [(rule, font)]."""
    family = family.lower() if family else None
    denied = [r for r in rules if not r.allow and size_allowed(r.sizes, size)]
    out, seen = [], set()
    for rule in rules:
        if not rule.allow or not size_allowed(rule.sizes, size):
            continue
        found = [f for f in fonts if matches(rule.pattern, f) and not any(matches(d.pattern, f) for d in denied) and (f.mono or not mono)
                 and (not family or matches(family, f)) and f.file not in seen and not missing_letters(charset(f), letters)]
        found.sort(key=lambda f: (style_rank(f.style), f.order))
        for f in found:
            seen.add(f.file)
            out.append((rule, f))
    return out


def fc(command):
    try:
        run = subprocess.run(command, capture_output=True, text=True)
    except OSError as e:
        raise FontsError(f"{command[0]}: {e.strerror}: fontconfig is required to find fonts")
    if run.returncode:
        raise FontsError(f"{' '.join(command[:2])}: {run.stderr.strip()}")
    return run.stdout


def read_fc(text, order0=0, every_face=False):
    """Faces as fontconfig lists them; the first face of a file only, which font2c makes, unless every_face."""
    fonts = []
    for line in text.splitlines():
        parts = line.split("|")
        if len(parts) != 7 or not parts[0].lower().endswith(FONT_FILES):
            continue
        if parts[1] != "0" and not every_face:
            continue
        fonts.append(Font(parts[0], int(parts[1]), parts[2], parts[3], parts[4] == "100", parts[5], parts[6], order0 + len(fonts)))
    return fonts


def scan(files, order0):
    """The faces of font files by fontconfig."""
    fonts = []
    for k in range(0, len(files), 200):
        fonts += read_fc(fc(["fc-scan", "--format", FC_FORMAT] + files[k:k + 200]), order0 + len(fonts))
    return fonts


def gather(sources):
    """The faces of the sources of the rules, in their order, each file once."""
    fonts, seen = [], set()
    for kind, path in sources:
        if kind == "system":
            got = sorted(read_fc(fc(["fc-list", "--format", FC_FORMAT])), key=lambda f: f.file)
        elif kind == "dir":
            files = sorted(os.path.join(d, n) for d, _, names in os.walk(path) for n in names if n.lower().endswith(FONT_FILES))
            got = scan(files, 0)
        else:
            try:
                lines = open(path, encoding="utf-8-sig").read().splitlines()
            except OSError as e:
                raise FontsError(f"source list {path}: {e.strerror}")
            here = os.path.dirname(os.path.abspath(path))
            files = [os.path.expanduser(l.strip()) for l in lines if l.strip() and not l.strip().startswith("#")]
            files = [f if os.path.isabs(f) else os.path.normpath(os.path.join(here, f)) for f in files]
            got = scan([f for f in files if os.path.exists(f)], 0)
        for f in got:
            if f.file not in seen:
                seen.add(f.file)
                fonts.append(f._replace(order=len(fonts)))
    return fonts


def face_by_postscript(name, where):
    """The face of this PostScript name, exactly, as the first face of its file."""
    found = read_fc(fc(["fc-list", f":postscriptname={name}", "--format", FC_FORMAT]), every_face=True)
    found = [f for f in found if f.psname == name]
    if not found:
        raise FontsError(f"{where}: no installed face has the PostScript name {name} (fc-list ':postscriptname={name}')")
    first = [f for f in found if f.index == 0]
    if not first:
        raise FontsError(f"{where}: {name} is face {found[0].index} of the collection {found[0].file}; font2c renders only the first face of a collection")
    return first[0]


def face_of_file(path, where):
    """The first face of a font file."""
    found = scan([path], 0)
    if not found:
        raise FontsError(f"{where}: fontconfig does not recognize {path} as a font")
    return found[0]


NAME_IDS = {0: "copyright", 5: "version", 13: "license", 14: "license URL"}


def name_table(path):
    """What a font file says of itself in its name table: {copyright, version, license, license URL}, those it has, in
    English when it has several languages; empty for a file that is not a TrueType or OpenType font."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        start = struct.unpack_from(">I", data, 12)[0] if data[:4] == b"ttcf" else 0  # a collection: its first face
        for k in range(struct.unpack_from(">H", data, start + 4)[0]):
            tag, _, offset, _ = struct.unpack_from(">4sIII", data, start + 12 + 16 * k)
            if tag == b"name":
                break
        else:
            return {}
        _, count, strings = struct.unpack_from(">HHH", data, offset)
        found = {}
        for k in range(count):
            platform, encoding, language, name_id, length, at = struct.unpack_from(">HHHHHH", data, offset + 6 + 12 * k)
            if name_id not in NAME_IDS:
                continue
            raw = data[offset + strings + at:offset + strings + at + length]
            if platform in (0, 3):  # Unicode, Windows: UTF-16; Windows English first
                text, rank = raw.decode("utf-16-be", "replace"), 0 if platform == 0 or language == 0x409 else 1
            elif platform == 1 and encoding == 0:  # Macintosh Roman
                text, rank = raw.decode("mac_roman", "replace"), 2
            else:
                continue
            text = " ".join(text.split())
            if text and (name_id not in found or rank < found[name_id][0]):
                found[name_id] = (rank, text)
        return {NAME_IDS[i]: text for i, (_, text) in sorted(found.items())}
    except (OSError, struct.error):
        return {}


# ----------------------------------------------------------------------------------------------------------------
#  The letters of texts
# ----------------------------------------------------------------------------------------------------------------

def po_strings(text):
    """The strings of a .po or .pot file, the header entry (an empty msgid) aside."""
    entries, entry, kind = [], None, None
    for raw in text.splitlines():
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        m = re.match(r'^(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+"(.*)"$', s)
        if m:
            kind = m.group(1)
            if kind == "msgid":
                entry = {}
                entries.append(entry)
            if entry is not None:
                entry.setdefault(kind, []).append(m.group(2))
        elif s.startswith('"') and s.endswith('"') and kind and entry is not None:
            entry.setdefault(kind, []).append(s[1:-1])
    out = []
    for e in entries:
        if "".join(e.get("msgid", [])) == "":
            continue
        for k, pieces in e.items():
            if k != "msgctxt":
                out.append(unescape("".join(pieces)))
    return out


PO_ESCAPES = {'"': '"', "\\": "\\", "'": "'", "?": "?", "n": "\n", "r": "\r", "t": "\t", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def unescape(s):
    """The escapes of a string of a .po file, as gettext takes them: those of C, octal and hexadecimal ones included; a
    backslash before any other character stands for that character."""
    def one(m):
        e = m.group(1)
        if e[0] == "x":
            return chr(int(e[1:], 16))
        if e[0] in "01234567":
            return chr(int(e, 8))
        return PO_ESCAPES.get(e, e)
    return re.sub(r'\\(x[0-9a-fA-F]+|[0-7]{1,3}|.)', one, s, flags=re.S)


def json_strings(value):
    if isinstance(value, str):
        return [value]
    if isinstance(value, dict):
        return [s for v in value.values() for s in json_strings(v)]
    if isinstance(value, list):
        return [s for v in value for s in json_strings(v)]
    return []


def printable(chars):
    """The characters a font can have: everything but control and format characters and the line and paragraph
    separators. The icons of an icon font live in the Private Use Area, which Python's isprintable() would drop."""
    return {c for c in chars if unicodedata.category(c) not in ("Cc", "Cf", "Cs", "Zl", "Zp")}


def letters_of_file(path, where):
    """The letters a text gives: the strings of a .po or .pot, the strings of a JSON, any other file whole."""
    try:
        text = open(path, encoding="utf-8-sig").read()
    except OSError as e:
        raise FontsError(f"{where}: {path}: {e.strerror}")
    except UnicodeDecodeError:
        raise FontsError(f"{where}: {path} is not UTF-8")
    low = path.lower()
    if low.endswith((".po", ".pot")):
        return printable("".join(po_strings(text)))
    if low.endswith(".json"):
        try:
            data = json.loads(text)
        except ValueError as e:
            raise FontsError(f"{where}: {path}: {e}")
        return printable("".join(json_strings(data)))
    return printable(text)


# ----------------------------------------------------------------------------------------------------------------
#  Fonts as C: what font2c writes, what DGX bundles, what a program has
# ----------------------------------------------------------------------------------------------------------------

def strip_comments(c_text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", "", c_text, flags=re.S)


def font_blocks(c_text):
    """The fonts of a C file, by the function of each: [(name, text of its part)]."""
    ends = [(m.group(1), m.end()) for m in re.finditer(r"dgx_font_t\s*\*\s*(\w+)\s*\(\s*(?:void)?\s*\)\s*\{", c_text)]
    blocks, start = [], 0
    for name, end in ends:
        close = c_text.find("\n}", end)
        close = len(c_text) if close < 0 else close + 2
        blocks.append((name, c_text[start:close]))
        start = close
    return blocks


def glyph_ranges(c_text, where):
    """The code points a font's C file has, by its glyph_ranges[] (every font of DGX has one)."""
    m = re.search(r"glyph_array_t\s+\w*glyph_ranges\s*\[\s*\]\s*=\s*\{(.*?)\};", strip_comments(c_text), re.S)
    if not m:
        raise FontsError(f"{where}: no glyph_ranges[] in it: not a DGX font")
    points = set()
    for first, number in re.findall(r"\{\s*(0[xX][0-9a-fA-F]+|\d+)\s*,\s*(0[xX][0-9a-fA-F]+|\d+)\s*,", m.group(1)):
        first, number = int(first, 0), int(number, 0)
        points.update(range(first, first + number))
    return points


def accessor(c_text, where):
    """The function a font's C file gives it by: dgx_font_t *Name()."""
    names = [name for name, _ in font_blocks(c_text)]
    if not names:
        raise FontsError(f"{where}: no dgx_font_t *<name>() in it: not a DGX font")
    return names[0]


def hw_code_points(path, where):
    """The code points of a handwritten font's JSON (the hw-fonts editor)."""
    try:
        data = json.load(open(path, encoding="utf-8-sig"))
    except (OSError, ValueError) as e:
        raise FontsError(f"{where}: {path}: {e}")
    points = data.get("codePoints") if isinstance(data, dict) else None
    if not isinstance(points, dict):
        raise FontsError(f"{where}: {path} has no \"codePoints\": not a handwritten font")
    out = set()
    for key in points:
        try:
            out.add(int(key))
        except ValueError:
            pass
    return out


def hw_names(path):
    """The names of a handwritten font: the one in its JSON and that of its file."""
    names = [os.path.splitext(os.path.basename(path))[0]]
    try:
        name = json.load(open(path, encoding="utf-8-sig")).get("name")
    except (OSError, ValueError, AttributeError):
        name = None
    return ([name] if isinstance(name, str) and name else []) + names


Origin = namedtuple("Origin", "family source license")


def dgx_origins(dgx, cache):
    """What DGX's own fonts were made from and under what license, by function: the table of src/fonts/README.md."""
    if "origins" not in cache:
        origins = {}
        try:
            text = open(os.path.join(dgx, "src", "fonts", "README.md"), encoding="utf-8").read()
        except OSError:
            text = ""
        for line in text.splitlines():
            cells = [c.strip() for c in line.strip().strip("|").split("|")]
            if line.startswith("|") and len(cells) == 4 and re.fullmatch(r"`\w+`", cells[0]):
                origins[cells[0].strip("`")] = Origin(*cells[1:])
        cache["origins"] = origins
    return cache["origins"]


def list_fonts(path):
    """The font files a list of font2c names, read as font2c reads it: [(where, file, size in pixels, or None for a
    handwritten font's JSON)]. A line font2c would refuse is left to font2c to tell."""
    try:
        lines = open(path, encoding="utf-8-sig").read().splitlines()
    except OSError as e:
        raise FontsError(f"{path}: {e.strerror}")
    here, out = os.path.dirname(os.path.abspath(path)), []
    for number, line in enumerate(lines, 1):
        words = []
        for m in re.finditer(r'"([^"]*)"|(\S+)', line):
            if m.group(2) is not None and m.group(2).startswith("#"):
                break
            words.append(m.group(1) if m.group(1) is not None else m.group(2))
        rest = []
        k = 0
        while k < len(words):
            if words[k] in ("-n", "-f", "-g"):
                k += 2
                continue
            rest.append(words[k])
            k += 1
        if not rest:
            continue
        file = rest[0] if os.path.isabs(rest[0]) else os.path.join(here, rest[0])
        if file.lower().endswith(".json"):
            out.append((f"{path}:{number}", file, None))
        elif len(rest) > 1 and rest[1].isdigit() and os.path.exists(file):
            out.append((f"{path}:{number}", file, int(rest[1])))
    return out


def write_if_changed(path, text):
    """A file written only when it is not already so: a build does not make again what did not change."""
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    aside = path + ".tmp"
    with open(aside, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(aside, path)  # whole or not at all, should the build be stopped halfway
    return True


def font2c_binary(dgx, outdir):
    """DGX's font2c, built from its source into outdir when the source is newer (FreeType needed)."""
    source = os.path.join(dgx, "font2c", "font2c.c")
    built = os.path.join(outdir, "font2c.exe" if os.name == "nt" else "font2c")
    if not os.path.exists(source):
        raise FontsError(f"no font2c source in {dgx}: not a DGX component")
    if os.path.exists(built) and os.path.getmtime(built) >= os.path.getmtime(source):
        return built
    # the host's compiler with the host's tools: inside an ESP-IDF build the PATH leads to the chips' tools first
    env = dict(os.environ, PATH=os.pathsep.join(p for p in os.environ.get("PATH", "").split(os.pathsep) if ".espressif" not in p))
    compiler = os.environ.get("CC", "")
    if not compiler or ".espressif" in compiler or "-esp" in os.path.basename(compiler):  # a CC of the chip is no use here
        compiler = "gcc" if os.name == "nt" else "cc"  # MSYS2 and MinGW have gcc, not cc
    try:
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "freetype2"], capture_output=True, text=True, env=env)
    except OSError:
        raise FontsError("pkg-config is not installed; it is required to build font2c against FreeType (libfreetype-dev)")
    if flags.returncode:
        raise FontsError("the FreeType development files (libfreetype-dev) are not installed, so font2c cannot be built: " + flags.stderr.strip())
    try:
        run = subprocess.run([compiler, "-O2", source, "-o", built] + flags.stdout.split(), capture_output=True, text=True, env=env)
    except OSError:
        raise FontsError(f"no host C compiler ({compiler}; set CC to another) to build font2c with")
    if run.returncode:
        raise FontsError("font2c failed to build:\n" + run.stderr.strip())
    return built


# ----------------------------------------------------------------------------------------------------------------
#  The needs
# ----------------------------------------------------------------------------------------------------------------

Need = namedtuple("Need", "name words where place")
Board = namedtuple("Board", "scale pixels mm")


def parse_needs(text, name="needs", base=None):
    """The needs file: the DGX directory, the board, the rules file and the needs. A relative path is taken from `base`."""
    dgx, rules, scale, pixels, mm, needs, current, lists = None, None, None, None, None, [], None, []

    def path_of(value):
        return os.path.normpath(os.path.join(base, os.path.expanduser(value))) if base and not os.path.isabs(os.path.expanduser(value)) else value

    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        where = f"{name}:{number}"
        key, _, value = line.partition(" ")
        if key in ("letters", "except"):
            value = raw.rstrip("\r\n").lstrip()[len(key) + 1:]  # the characters as written, spaces among them
        else:
            value = value.strip()
        if key == "need":
            value, _, place = value.partition(" ")
            place = place.strip() or where
            if not re.fullmatch(r"[A-Za-z_]\w*", value):
                raise FontsError(f"{where}: the name of a need must be a C identifier, not '{value}'")
            current = Need(value, [], where, place)
            needs.append(current)
        elif key == "component":
            if dgx:
                raise FontsError(f"{where}: component given twice")
            dgx = path_of(value)
        elif key == "rules":
            if rules and rules != path_of(value):
                raise FontsError(f"{where}: rules given twice: {rules} and {path_of(value)}")
            rules = path_of(value)
        elif key == "list":
            lists.append(path_of(value))
        elif key == "scale":
            try:
                if scale is not None and scale != float(value):
                    raise FontsError(f"{where}: scale given twice: {scale:g} and {value}")
                scale = float(value)
                if scale <= 0:
                    raise ValueError
            except ValueError:
                raise FontsError(f"{where}: scale <n>, display pixels per point of the frame, not '{value}'")
        elif key == "board":
            kind, _, rest = value.partition(" ")
            parts = rest.split()
            try:
                if kind in ("pixels", "mm") and len(parts) == 2:
                    pair = (float(parts[0]), float(parts[1]))
                    if kind == "pixels":
                        if pixels and pixels != pair:
                            raise FontsError(f"{where}: board pixels given twice: {pixels[0]:g} {pixels[1]:g} and {rest}")
                        pixels = pair
                    else:
                        if mm and mm != pair:
                            raise FontsError(f"{where}: board mm given twice: {mm[0]:g} {mm[1]:g} and {rest}")
                        mm = pair
                else:
                    raise ValueError
                if (pixels and min(pixels) <= 0) or (mm and min(mm) <= 0):
                    raise ValueError
            except ValueError:
                raise FontsError(f"{where}: board pixels <w> <h> or board mm <w> <h>, not '{value}'")
        elif current is None:
            raise FontsError(f"{where}: '{key}' comes before any need")
        elif key in ("text", "file", "source"):
            current.words.append((key, path_of(value), where))
        else:
            current.words.append((key, value, where))
    if not dgx:
        dgx = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    return dgx, rules, Board(scale if scale is not None else 1.0, pixels, mm), needs, lists


# what a need comes to
Made = namedtuple("Made", "need face px letters how mm by")        # a bitmap font to make: face a Font, how the size came, by
                                                                   # what the face was chosen
Hand = namedtuple("Hand", "need source letters")                   # a handwritten font to make, of its JSON
AsIs = namedtuple("AsIs", "need path function how letters")        # a font taken as it is; how: dgx or file


def pixels_per_mm(board, where):
    if not board.pixels or not board.mm:
        raise FontsError(f"{where}: a size in mm needs the display size: declare dgx_fonts_board(PIXELS w h MM w h) in the program's CMakeLists")
    across, down = board.pixels[0] / board.mm[0], board.pixels[1] / board.mm[1]
    if abs(across - down) > 0.05 * across:
        raise FontsError(f"{where}: the display's pixels and millimeters do not agree: {across:.2f} px/mm across, {down:.2f} px/mm down")
    return across


def size_in_px(text, board, where):
    """A size as pixels of the display: '12pt' (points of the frame, times the scale), '4.5mm', '30px' or '30'."""
    m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*(pt|mm|px)?", text)
    if not m:
        raise FontsError(f"{where}: a size is a number followed by pt, mm or px, not '{text}'")
    value, unit = float(m.group(1)), m.group(2) or "pt"
    if unit == "pt":
        px = value * board.scale
    elif unit == "mm":
        px = value * pixels_per_mm(board, where)
    else:
        px = value
    px = int(math.floor(px + 0.5))  # halves go up: 12.5 px is 13, not the even 12 of round()
    if px < 1 or px > 1000:
        raise FontsError(f"{where}: {text} comes out as {px} px, which is out of range")
    how = f"{value:g} pt x {board.scale:g}" if unit == "pt" else f"{value:g} mm" if unit == "mm" else f"{px} px"
    return px, value if unit == "mm" else None, how


class Declared:
    """A need as declared: its keywords and the characters it asks for. Declarations of one name with the same keywords
    are merged into one of these."""

    def __init__(self, need, fields, letters):
        self.need, self.fields, self.letters = need, fields, letters


def need_fields(need):
    """The keywords of a declaration other than its characters: {key: (value, where)}."""
    fields = {}
    for key, value, where in need.words:
        if key in ("text", "letters", "except"):
            continue
        if key not in ("size", "mono", "family", "postscript", "dgx", "file", "source"):
            raise FontsError(f"{where}: unknown keyword '{key}' for the need {need.name}")
        if key in fields:
            raise FontsError(f"{where}: {key} given twice for the need {need.name}")
        fields[key] = (value, where)
    return fields


def need_letters(need):
    """The characters a declaration asks for: those of its texts and its letters, less its except."""
    letters, spared = set(), set()
    for key, value, where in need.words:
        if key == "text":
            letters |= letters_of_file(value, where)
        elif key == "letters":
            letters |= printable(value)
        elif key == "except":
            spared |= set(value)
    return letters - spared


def merge_needs(needs):
    """Declarations of one name with the same keywords are one need with the characters of all of them, so that its face
    is picked once, for every character; declarations with different keywords stay apart and must come to the same
    font (join_needs)."""
    merged, index = [], {}
    for n in needs:
        fields = need_fields(n)
        key = (n.name, tuple(sorted((k, v) for k, (v, _) in fields.items())))
        if key in index:
            index[key].letters |= need_letters(n)
        else:
            index[key] = Declared(n, fields, need_letters(n))
            merged.append(index[key])
    return merged


def resolve(declared, board, rules, dgx, cache):
    """What a declared need comes to: a Made, a Hand or an AsIs. rules: the program's Rules, or None."""
    need, fields, letters = declared.need, declared.fields, declared.letters
    ways = [k for k in ("dgx", "file", "source") if k in fields]
    if len(ways) > 1:
        raise FontsError(f"{need.where}: {need.name}: only one of dgx, file or source, not {' and '.join(ways)}")
    way = ways[0] if ways else "face"
    size_text, mm, face = fields.get("size", (None, None))[0], None, None
    exact = [k for k in ("postscript", "source") if k in fields]
    if exact and way != "dgx" and way != "file":
        loose = [k for k in ("mono", "family") if k in fields]
        if loose or len(exact) > 1:
            raise FontsError(f"{need.where}: {need.name}: {exact[0]} names the face itself; {' and '.join(loose or exact[1:])} do not apply")
    if way in ("dgx", "file") and ("size" in fields or "mono" in fields or "family" in fields or "postscript" in fields):
        raise FontsError(f"{need.where}: {need.name}: an existing font has its own size and face: size, mono, family and postscript do not apply")
    if way == "dgx":
        name, where = fields["dgx"]
        path = os.path.join(dgx, "src", "fonts", name + ".c")
        if not os.path.exists(path):
            have = sorted(os.path.splitext(f)[0] for f in os.listdir(os.path.join(dgx, "src", "fonts")) if f.endswith(".c"))
            raise FontsError(f"{where}: DGX has no font {name}; it has {', '.join(have)}")
        c_text = open(path, encoding="utf-8", errors="replace").read()
        function = accessor(c_text, f"{where}: {path}")
        check_letters(glyph_ranges(c_text, f"{where}: {path}"), letters, f"{where}: {name} of DGX")
        origin = dgx_origins(dgx, cache).get(function)
        admit(rules, f"{need.place}: {need.name} asks for {function} of DGX", names=[function] + ([origin.family] if origin else []))
        return AsIs(need, path, function, "dgx", letters)
    if way == "file":
        path, where = fields["file"]
        try:
            c_text = open(path, encoding="utf-8", errors="replace").read()
        except OSError as e:
            raise FontsError(f"{where}: {path}: {e.strerror}")
        function = accessor(c_text, f"{where}: {path}")
        check_letters(glyph_ranges(c_text, f"{where}: {path}"), letters, f"{where}: {path}")
        admit(rules, f"{need.place}: {need.name} asks for {function} of {path}", names=[function])
        return AsIs(need, path, function, "file", letters)
    if way == "source":
        path, where = fields["source"]
        if path.lower().endswith(".json"):
            if "size" in fields:
                raise FontsError(f"{where}: {need.name}: a handwritten font has no size; the size is chosen when text is drawn")
            check_letters(hw_code_points(path, where), letters, f"{where}: {path}")
            admit(rules, f"{need.place}: {need.name} asks for the handwritten font {path}", names=hw_names(path))
            return Hand(need, path, letters)
        face, by = face_of_file(path, where), f"named by its file in {need.place}"
    if not letters:
        raise FontsError(f"{need.where}: {need.name}: no characters: give it texts or letters")
    if size_text is None:
        raise FontsError(f"{need.where}: {need.name}: no size")
    px, mm, how = size_in_px(size_text, board, need.where)
    if face is None and "postscript" in fields:
        name, where = fields["postscript"]
        face, by = face_by_postscript(name, where), f"named by its PostScript name in {need.place}"
    if face is not None:
        admit(rules, f"{need.place}: {need.name} asks for {face.family} {face.style} ({face.file}) at {px} px", px, face)
        check_letters(charset(face), letters, f"{need.where}: {face.family} {face.style} ({face.file})")
        return Made(need, face, px, letters, how, mm, by)
    # by the rules
    if not rules:
        raise FontsError(f"{need.where}: {need.name} asks for a face by family or by the rules, and the program declared no rules: "
                         f"add dgx_fonts_rules(<file>) to its CMakeLists, or name a PostScript face")
    if "faces" not in cache:
        if not rules.sources:
            raise FontsError(f"{rules.path}: no source of fonts (source system, source dir or source list) for the needs the rules pick a face for")
        cache["faces"] = gather(rules.sources)
    mono = fields.get("mono", ("0", None))[0] not in ("0", "")
    family = fields.get("family", (None, None))[0]
    found = candidates(cache["faces"], rules.rules, px, mono, family, "".join(sorted(letters)))
    if not found:
        raise FontsError(f"{need.where}: {need.name}: no face allowed by the rules at {px} px{' (mono)' if mono else ''}"
                         f"{' of ' + family if family else ''} has all the characters: {''.join(sorted(letters))}")
    rule, face = found[0]
    return Made(need, face, px, letters, how, mm, f"picked by the rules, {rule.where}")


def check_letters(points, letters, where):
    missing = missing_letters(points, sorted(letters - {" "}))
    if missing:
        shown = " ".join(f"{c} U+{ord(c):04X}" for c in missing[:12]) + (" ..." if len(missing) > 12 else "")
        raise FontsError(f"{where}: lacks the characters {shown}")


def c_name(text):
    return re.sub(r"[^A-Za-z0-9]", "", text)


def api_names(dgx):
    """The identifiers DGX's headers have, so that no need takes the name of one."""
    names = set()
    for root, _, files in os.walk(os.path.join(dgx, "include")):
        for f in files:
            if f.endswith(".h"):
                names.update(re.findall(r"\bdgx_font_\w+", open(os.path.join(root, f), encoding="utf-8", errors="replace").read()))
    return names


# ----------------------------------------------------------------------------------------------------------------
#  The build
# ----------------------------------------------------------------------------------------------------------------

def list_word(path):
    """A path as a word of a list of font2c: in quotes when it has a space."""
    if '"' in path:
        raise FontsError(f"{path}: a path with a quote in it cannot be passed to font2c")
    return f'"{path}"' if " " in path or "\t" in path else path


def font_points(c_text):
    """The code points of every function of a C file font2c -jo wrote, by name; a function giving another's font has its points."""
    points, aliases = {}, {}
    for name, text in font_blocks(c_text):
        m = re.search(r"return\s+(\w+)\s*\(\s*\)\s*;", text[text.rindex("dgx_font_t"):])
        if m and "glyph_ranges" not in text[text.rindex("dgx_font_t"):]:
            aliases[name] = m.group(1)
        else:
            points[name] = glyph_ranges(text, name)
    for name, of in aliases.items():
        points[name] = points.get(of, set())
    return points


def same_font(a, b):
    """Whether two needs came to one font."""
    if type(a) is not type(b):
        return False
    if isinstance(a, Made):
        return a.face.file == b.face.file and a.px == b.px
    if isinstance(a, Hand):
        return a.source == b.source
    return a.how == b.how and a.function == b.function and (a.how == "dgx" or a.path == b.path)


def describe(w):
    if isinstance(w, Made):
        return f"{w.face.family} {w.face.style} at {w.px} px"
    if isinstance(w, Hand):
        return f"the handwritten font {os.path.basename(w.source)}"
    return f"the existing font {w.function}"


def join_needs(wants):
    """Needs of one name told in several places: one need with the letters of all, when they came to one font."""
    first, joined = {}, []
    for w in wants:
        name = w.need.name
        if name in first:
            if not same_font(first[name], w):
                raise FontsError(f"the need {name} is declared twice with different fonts: {describe(first[name])} in {first[name].need.place}, "
                                 f"{describe(w)} in {w.need.place}")
            first[name].letters.update(w.letters)
        else:
            first[name] = w
            joined.append(w)
    return joined


USED_HEAD = """\
# The fonts of this program: where each one came from, what its file says of its license, and which needs and
# lists use it. Generated by dgx_fonts.py of DGX each time the fonts are made. Do not edit.
#
# Make sure you may use every font listed here: DGX takes the fonts the program and its components ask for and
# does not judge their licenses. To keep a font out, deny it in the program's rules (deny "Family*"): the rules
# then never pick it, and a need or a list that names it exactly stops the build. The word strict in the rules
# lets in only the fonts their allow lines name. See "Fonts and their licenses" in docs/fonts-en.md of DGX.
"""


def shortened(text, most=300):
    """A long text cut at a word, saying so."""
    if len(text) <= most:
        return text
    return text[:text.rfind(" ", 0, most)] + " ... (shortened; the whole text is in the font file)"


def fonts_used(resolved, listed, dgx, cache):
    """The text of fonts-used.txt: every font of the needs and the lists, once, with what it is and who uses it."""
    fonts = {}

    def entry(key, title, lines):
        if key not in fonts:
            fonts[key] = (title, lines, [])
        return fonts[key][2]

    def of_file(path):
        info = name_table(path)
        return [f"{k}: {shortened(info[k])}" for k in ("version", "copyright", "license", "license URL") if k in info] or \
            ["the file says nothing of its copyright or license"]

    for w in resolved:
        if isinstance(w, Made):
            f = w.face
            uses = entry(("face", f.file), f"{f.family} {f.style}", [f"file: {f.file}, PostScript name {f.psname}"] + of_file(f.file))
            uses.append(f"{w.need.name} at {w.px} px, {w.by}")
        elif isinstance(w, Hand):
            entry(("hand", w.source), f"the handwritten font {hw_names(w.source)[0]}", [f"file: {w.source}"]).append(
                f"{w.need.name}, named by its file in {w.need.place}")
        elif w.how == "dgx":
            origin = dgx_origins(dgx, cache).get(w.function)
            about = [f"made from: {origin.source}", f"license: {origin.license}"] if origin else \
                ["what it was made from is not recorded in src/fonts/README.md of DGX"]
            entry(("dgx", w.function), f"{w.function}, a font of DGX", [f"file: {w.path}"] + about).append(
                f"{w.need.name}, used as is, in {w.need.place}")
        else:
            entry(("file", w.path), f"{w.function}, a font of the program", [f"file: {w.path}",
                  "a font as C: what it was made from and under what license is for the program to know"]).append(
                f"{w.need.name}, used as is, in {w.need.place}")
    for where, file, px in listed:
        if px is None:
            entry(("hand", file), f"the handwritten font {hw_names(file)[0]}", [f"file: {file}"]).append(f"a list, {where}")
        else:
            face = face_of_file(file, where)
            entry(("face", file), f"{face.family} {face.style}", [f"file: {file}, PostScript name {face.psname}"] + of_file(file)).append(
                f"a list at {px} px, {where}")
    out = [USED_HEAD]
    for title, lines, uses in fonts.values():
        out.append(title)
        out += [f"  {line}" for line in lines]
        out += [f"  used by: {use}" for use in dict.fromkeys(uses)]
        out.append("")
    return "\n".join(out)


def build(needs_path, outdir, scale=None, pixels=None, mm=None, rules=None, component=None):
    """The fonts of a needs file into outdir; the board and the rules may come as arguments, for a host build, and win."""
    outdir = os.path.abspath(outdir)
    try:
        text = open(needs_path, encoding="utf-8-sig").read()
    except OSError as e:
        raise FontsError(f"{needs_path}: {e.strerror}")
    dgx, rules_path, board, needs, lists = parse_needs(text, needs_path, os.path.dirname(os.path.abspath(needs_path)))
    if scale is not None or pixels or mm:
        board = Board(scale if scale is not None else board.scale, tuple(pixels) if pixels else board.pixels, tuple(mm) if mm else board.mm)
    if rules:
        rules_path = os.path.abspath(rules)
    if component:
        dgx = os.path.abspath(component)
    if not needs and not lists:
        raise FontsError(f"{needs_path}: no needs")
    taken = api_names(dgx)
    for n in needs:
        if f"dgx_font_{n.name}" in taken:
            raise FontsError(f"{n.where}: dgx_font_{n.name} is already a function of DGX; choose another name for the need")
    by_upper = {}
    for n in needs:
        other = by_upper.setdefault(n.name.upper(), n)
        if other.name != n.name:
            raise FontsError(f"{n.where}: the needs {other.name} ({other.where}) and {n.name} differ only in case; their macros would not")
    cache = {}
    program_rules = read_rules(rules_path) if rules_path else None
    resolved = [resolve(d, board, program_rules, dgx, cache) for d in merge_needs(needs)]
    for path in lists:
        if not os.path.exists(path):
            raise FontsError(f"{needs_path}: the list {path} does not exist")
    listed = [(where, file, px) for path in lists for where, file, px in list_fonts(path)]
    for where, file, px in listed:
        if px is None:
            admit(program_rules, f"{where}: the list asks for the handwritten font {file}", names=hw_names(file))
        else:
            face = face_of_file(file, where)
            admit(program_rules, f"{where}: the list asks for {face.family} {face.style} ({file}) at {px} px", px, face)
    report = os.path.join(outdir, "fonts-used.txt")
    wants = join_needs(resolved)
    made = [w for w in wants if not isinstance(w, AsIs)]
    os.makedirs(outdir, exist_ok=True)
    work = os.path.join(outdir, "work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    # the needs as a list of font2c, a line a need, with its letters in a file of its own
    lines = []
    for w in made:
        words = ["-n", f"dgx_font_{w.need.name}"]
        if w.letters:
            letters = os.path.join(work, f"{w.need.name}.letters")
            with open(letters, "w", encoding="utf-8") as f:
                f.write("".join(sorted(w.letters)))
            words += ["-f", list_word(letters)]
        if isinstance(w, Made):
            words += [list_word(w.face.file), str(w.px)]
        else:
            words.append(list_word(w.source))
        lines.append(" ".join(words))
    counts, c_text, h_text = {}, "", ""
    if lines or lists:
        tool = font2c_binary(dgx, outdir)
        command = [tool]
        if lines:
            own = os.path.join(work, "needs.list")
            with open(own, "w", encoding="utf-8") as f:
                f.write("# the needs of the program, as dgx_fonts.py gave them to font2c\n" + "\n".join(lines) + "\n")
            command += ["-jo", own]
        for path in lists:
            command += ["-jo", path]
        command += ["-o", os.path.join(work, "dgx_fonts")]
        run = subprocess.run(command, capture_output=True, text=True)
        if run.returncode:
            raise FontsError(f"font2c failed: {run.stderr.strip() or run.stdout.strip()}")
        for m in re.finditer(r"^(\w+): .* (\d+) (?:glyphs|symbols)$", run.stdout, re.M):
            counts[m.group(1)] = int(m.group(2))
        c_text = open(os.path.join(work, "dgx_fonts.c"), encoding="utf-8", errors="replace").read()
        h_text = open(os.path.join(work, "dgx_fonts.h"), encoding="utf-8", errors="replace").read()
        points = font_points(c_text)
        for w in made:
            name = f"dgx_font_{w.need.name}"
            if name not in points:
                raise FontsError(f"{w.need.where}: font2c did not produce {name}")
            check_letters(points[name], w.letters, f"{w.need.where}: font2c of {w.face.file if isinstance(w, Made) else w.source}")
            counts.setdefault(name, len(points[name]))
        for name, pts in points.items():  # a function of a list that shares a font is counted by that font
            counts.setdefault(name, len(pts))
    functions = re.findall(r"^dgx_font_t \*(\w+)\(void\);$", h_text, re.M)
    own_names = {f"dgx_font_{w.need.name}" for w in made}
    told = []
    # the two files: the C as font2c wrote it, the header with the needs, the fonts as they are and the board
    c = ["/* Generated by dgx_fonts.py of DGX with font2c from the needs declared in the project's CMakeLists files. Do not edit. */",
         c_text.rstrip("\n") + "\n" if c_text else '#include "dgx_font.h"\n']
    h = ["#pragma once",
         "/* Generated by dgx_fonts.py of DGX from the needs declared in the project's CMakeLists files. Do not edit. */",
         "/* The fonts used, where each came from and what its file says of its license: fonts-used.txt beside this file. */",
         '#include "dgx_font.h"']
    if any(isinstance(w, Hand) for w in wants):
        h.append('#include "dgx_hw_font.h"')
    for name in sorted({os.path.splitext(os.path.basename(w.path))[0] for w in wants if isinstance(w, AsIs) and w.how == "dgx"}):
        h.append(f'#include "fonts/{name}.h"')
    h += ["", "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for w in wants:
        upper = w.need.name.upper()
        name = f"dgx_font_{w.need.name}"
        if isinstance(w, Made):
            h.append(f"/* {w.need.name}: {w.face.family} {w.face.style} at {w.px} px ({w.how}), made for this board */")
            h.append(f"dgx_font_t *{name}(void);")
            h.append(f"#define DGX_FONT_{upper}_MADE 1")
            h.append(f"#define DGX_FONT_{upper}_PX {w.px}")
            if w.mm is not None:
                h.append(f"#define DGX_FONT_{upper}_MM {w.mm:g}")
            told.append(f"{w.need.name}: {w.face.family} {w.face.style} {w.px} px ({w.how}), {counts.get(name, 0)} glyphs")
        elif isinstance(w, Hand):
            h.append(f"/* {w.need.name}: the handwritten font {os.path.basename(w.source)}; its size is chosen when text is drawn */")
            h.append(f"dgx_font_t *{name}(void);")
            h.append(f"#define DGX_FONT_{upper}_MADE 0")
            told.append(f"{w.need.name}: handwritten {w.source}, {counts.get(name, 0)} glyphs")
        else:
            what = f"{w.function} of DGX" if w.how == "dgx" else f"{os.path.basename(w.path)} of the program"
            h.append(f"/* {w.need.name}: {what}, used as is: its own function */")
            if w.how == "file":
                h.append(f"dgx_font_t *{w.function}(void); /* compiled by the program */")
            h.append(f"#define {name} {w.function}")
            h.append(f"#define DGX_FONT_{upper}_MADE 0")
            told.append(f"{w.need.name}: {what}, used as is")
        h.append("")
    others = [f for f in functions if f not in own_names]
    if others:
        h.append("/* the fonts of the lists, named as the lists name them */")
        for f in others:
            h.append(f"dgx_font_t *{f}(void);")
            told.append(f"{f}: from a list, {counts.get(f, 0)} glyphs")
        h.append("")
    h.append("/* every need, in declaration order: X(name) for each */")
    h.append("#define DGX_FONT_NEEDS(X) " + " ".join(f"X({w.need.name})" for w in wants))
    h.append("")
    h.append("/* the frame scale and the display the fonts were made for, as declared in the project's CMakeLists */")
    h.append(f"#define DGX_FONTS_SCALE {board.scale:g}")
    if board.pixels or board.mm:
        if board.pixels:
            h.append(f"#define DGX_FONTS_BOARD_WIDTH_PX {board.pixels[0]:g}")
            h.append(f"#define DGX_FONTS_BOARD_HEIGHT_PX {board.pixels[1]:g}")
        if board.mm:
            h.append(f"#define DGX_FONTS_BOARD_WIDTH_MM {board.mm[0]:g}")
            h.append(f"#define DGX_FONTS_BOARD_HEIGHT_MM {board.mm[1]:g}")
    h += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    shutil.rmtree(work, ignore_errors=True)
    write_if_changed(report, fonts_used(resolved, listed, dgx, cache))
    told.append(f"the fonts used and their licenses: {report}; make sure you may use every one of them")
    changed = write_if_changed(os.path.join(outdir, "dgx_fonts.h"), "\n".join(h))
    changed = write_if_changed(os.path.join(outdir, "dgx_fonts.c"), "\n".join(c)) or changed
    return told, changed


# ----------------------------------------------------------------------------------------------------------------
#  A font as JSON, for editors
# ----------------------------------------------------------------------------------------------------------------

def read_font(path, name=None):
    """A bitmap font of DGX (font2c's C) as a dict: the glyphs with their pixels as rows of '#' and '.'."""
    src = open(path, encoding="utf-8", errors="replace").read()
    blocks = font_blocks(src)
    if not blocks:
        raise FontsError(f"{path}: no dgx_font_t *<name>() in it")
    if name:
        found = [b for b in blocks if b[0] == name]
        if not found:
            raise FontsError(f"{path}: no font {name} in it; it has {', '.join(n for n, _ in blocks)}")
        name, text = found[0]
    else:
        name, text = blocks[0]
    m = re.search(r"(\w*)bitmaps\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        of = re.search(r"return\s+(\w+)\s*\(\s*\)\s*;", text[text.rindex("dgx_font_t"):])
        if of and of.group(1) != name:
            return read_font(path, of.group(1))
        raise FontsError(f"{path}: {name} has no bitmaps: not a bitmap font")
    prefix = m.group(1)
    bitmaps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", strip_comments(m.group(2)))]
    m = re.search(r"glyph_t\s+" + re.escape(prefix) + r"glyphs\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FontsError(f"{path}: {name} has no glyphs")
    glyphs = [tuple(int(v) for v in g) for g in
              re.findall(r"\{\s*\{\s*(?:\.bitmap\s*=\s*)?\w*bitmaps\s*\+\s*(\d+)\s*\}\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}",
                         strip_comments(m.group(1)))]
    m = re.search(r"glyph_array_t\s+" + re.escape(prefix) + r"glyph_ranges\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    if not m:
        raise FontsError(f"{path}: {name} has no glyph_ranges")
    ranges = [(int(a, 0), int(n, 0), int(k)) for a, n, k in
              re.findall(r"\{\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*\w*glyphs\s*\+\s*(\d+)\s*\}", strip_comments(m.group(1)))]
    fields = dict(re.findall(r"\.(\w+)\s*=\s*([-\w.]+)", text[text.rindex("dgx_font_t"):]))
    f_type = fields.get("f_type", "DGX_FONT_BITMAP_LINES")
    stream = f_type == "DGX_FONT_BITMAP_STREAM"
    font = {"name": name, "f_type": f_type, "yAdvance": int(fields["yAdvance"]), "yOffsetLowest": int(fields["yOffsetLowest"]),
            "xWidest": int(float(fields.get("xWidest", 0))), "glyphs": {}, "first_advance": glyphs[ranges[0][2]][3] if ranges else 0}
    for first, number, k in ranges:
        for i in range(number):
            at, w, h, adv, xo, yo = glyphs[k + i]
            rows = []
            for y in range(h):
                row = ""
                for x in range(w):
                    bit = (y * w + x) if stream else y * 8 * ((w + 7) // 8) + x
                    byte = bitmaps[at + bit // 8] if at + bit // 8 < len(bitmaps) else 0
                    row += "#" if byte >> (7 - bit % 8) & 1 else "."
                rows.append(row)
            font["glyphs"][str(first + i)] = {"w": w, "h": h, "xAdvance": adv, "xOffset": xo, "yOffset": yo, "rows": rows}
    return font


# ----------------------------------------------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description="The fonts of a program of DGX by its needs (font2c)")
    ap.add_argument("what", choices=("build", "pick", "list", "letters", "json"))
    ap.add_argument("args", nargs="*")
    ap.add_argument("--mono", action="store_true", help="pick, list: every letter as wide")
    ap.add_argument("--family", help="pick, list: only this family (a pattern, as in the rules)")
    ap.add_argument("--letters", help="pick, list: the letters the face must have")
    ap.add_argument("--font", help="json: the font of the file, by the name of its function")
    ap.add_argument("--scale", type=float, help="build: pixels of the display a point of the frame (sizes in pt)")
    ap.add_argument("--pixels", nargs=2, type=float, metavar=("W", "H"), help="build: the display in pixels (sizes in mm)")
    ap.add_argument("--mm", nargs=2, type=float, metavar=("W", "H"), help="build: the display in millimetres")
    ap.add_argument("--rules", help="build: the rules faces are picked by")
    ap.add_argument("--component", help="build: the DGX component, when not the one this script is in")
    a = ap.parse_args(argv)
    try:
        if a.what == "build":
            if len(a.args) != 2:
                raise FontsError("build NEEDS OUTDIR")
            told, changed = build(a.args[0], a.args[1], a.scale, a.pixels, a.mm, a.rules, a.component)
            for line in told:
                print(line)
            print("dgx_fonts.c and dgx_fonts.h " + ("written" if changed else "as they were"))
            return 0
        if a.what == "letters":
            found = set()
            for path in a.args:
                found |= letters_of_file(path, path)
            print("".join(sorted(found)))
            return 0
        if a.what == "json":
            if not a.args:
                raise FontsError("json FONT.c [OUT.json]")
            text = json.dumps(read_font(a.args[0], a.font), ensure_ascii=False)
            if len(a.args) > 1:
                open(a.args[1], "w", encoding="utf-8").write(text)
            else:
                print(text)
            return 0
        if len(a.args) != 2 or not a.args[1].isdigit():
            raise FontsError(f"{a.what} RULES SIZE")
        sources, rules, _ = parse_rules(open(a.args[0], encoding="utf-8-sig").read(), a.args[0], os.path.dirname(os.path.abspath(a.args[0])))
        if not sources:
            raise FontsError(f"{a.args[0]}: no source of fonts (source system, source dir or source list)")
        found = candidates(gather(sources), rules, int(a.args[1]), a.mono, a.family, a.letters or "")
        if a.what == "list":
            for rule, f in found:
                print(f"{f.family} {f.style}{' (mono)' if f.mono else ''}\t{f.file}\t{f.psname}\t({rule.where})")
            return 0
        if not found:
            raise FontsError(f"no face allowed by the rules at {a.args[1]} px{' (mono)' if a.mono else ''}{' of ' + a.family if a.family else ''}"
                             f"{' has the characters ' + a.letters if a.letters else ''}")
        rule, f = found[0]
        print(f"{f.family} {f.style}{' (mono)' if f.mono else ''}\t{f.file}\t{f.psname}\t({rule.where})")
        return 0
    except (FontsError, OSError) as e:
        print(f"dgx_fonts.py: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
