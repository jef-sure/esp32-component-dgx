#!/usr/bin/env python3
"""Builds test_deprecated.c as C and as C++ and checks that a deprecation warning comes on every line that ends with
"// deprecated" and on no other line, with no error and no other warning; a line that ends with "// deprecated in C" is
built as C only.

A warning a header gives is put on the line of the file that brought it there: the macro the line expands, or the
#include of the header.

    deprecated_check.py <cc> <c++> <file> <flags...>
"""
import os
import re
import subprocess
import sys


def lines_warned(compiler, lang, path, flags):
    run = subprocess.run([compiler, "-x", lang, "-fsyntax-only", "-fdiagnostics-plain-output", "-Wall", "-Wextra",
                          "-Wno-unused-parameter", "-Wdeprecated-declarations", *flags, path], capture_output=True, text=True)
    name = os.path.basename(path)
    warned, other = set(), []
    included = None
    pending = None  # a warning outside the file, waiting for the note that puts it on a line of the file

    def place(line):
        nonlocal pending
        if pending is not None:
            warned.add(line)
            pending = None

    for text in run.stderr.splitlines():
        m = re.match(r"In file included from (.+?):(\d+)", text)
        if m:
            if os.path.basename(m.group(1)) == name:
                included = int(m.group(2))
            continue
        m = re.match(r"(.+?):(\d+):\d+: (warning|error|note): (.*)", text)
        if not m:
            continue
        where, line, kind, what = os.path.basename(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
        if kind == "note":
            if where == name and "in expansion of macro" in what:
                place(line)
            continue
        if pending is not None and included is not None:
            place(included)
        if kind == "error" or "deprecated" not in what:
            other.append(text)
        elif where == name:
            warned.add(line)
        else:
            pending = text
        if where == name:
            included = None
    if pending is not None:
        if included is not None:
            warned.add(included)
        else:
            other.append(pending)
    return warned, other, run.returncode


def main():
    cc, cxx, path, flags = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    text = open(path, encoding="utf-8").read().splitlines()
    both = {i + 1 for i, line in enumerate(text) if line.rstrip().endswith("// deprecated")}
    c_only = {i + 1 for i, line in enumerate(text) if line.rstrip().endswith("// deprecated in C")}
    failed = False
    for compiler, lang, own in ((cc, "c", []), (cxx, "c++", ["-Wno-missing-field-initializers"])):
        flags_here = [f for f in flags if not (lang == "c++" and f.startswith("-std="))] + own
        marked = both | c_only if lang == "c" else both
        warned, other, code = lines_warned(compiler, lang, path, flags_here)
        for message in other:
            print(f"{lang}: not a deprecation: {message}")
        for line in sorted(marked - warned):
            print(f"{lang}: no warning on line {line}: {text[line - 1].strip()}")
        for line in sorted(warned - marked):
            print(f"{lang}: a warning on line {line}, which is not marked")
        if other or marked != warned or code != 0:
            failed = True
        print(f"{os.path.basename(path)} as {lang}: {len(warned & marked)} of {len(marked)} deprecated names warn")
    print("test_deprecated: " + ("FAILED" if failed else "ok"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
