#!/usr/bin/env python3
"""Put the version of idf_component.yml into the install lines of README.md.

    tools/readme_version.py           rewrite the lines that give another version
    tools/readme_version.py --check   change nothing; exit 1 if a line gives another version

The README installs DGX in two lines, `idf.py add-dependency "jef-sure/dgx^X.Y.Z"` and
`jef-sure/dgx: "^X.Y.Z"` in a manifest; both get the version of the manifest of this
repository. A README in which either line is missing is an error, so a line reworded
by hand is not left behind unnoticed. Runs on any Python 3, from any directory.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "idf_component.yml"
README = ROOT / "README.md"

VERSION = re.compile(r'^version:\s*"?(\d+\.\d+\.\d+)"?\s*$', re.MULTILINE)
LINES = (
    ("idf.py add-dependency", re.compile(r'(idf\.py add-dependency "jef-sure/dgx\^)(\d+\.\d+\.\d+)(")')),
    ("the manifest", re.compile(r'(jef-sure/dgx: "\^)(\d+\.\d+\.\d+)(")')),
)


def main(argv):
    check = argv[1:] == ["--check"]
    if argv[1:] and not check:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    found = VERSION.findall(MANIFEST.read_text(encoding="utf-8"))
    if len(found) != 1:
        print(f"{MANIFEST.name}: {len(found)} version lines, not one", file=sys.stderr)
        return 2
    version = found[0]
    text = README.read_text(encoding="utf-8")
    old = set()
    for name, pattern in LINES:
        given = pattern.findall(text)
        if not given:
            print(f"{README.name}: no install line of {name}", file=sys.stderr)
            return 2
        old.update(g[1] for g in given if g[1] != version)
        text = pattern.sub(lambda m: m.group(1) + version + m.group(3), text)
    if not old:
        print(f"{README.name} installs ^{version}")
        return 0
    if check:
        print(f"{README.name} installs ^{', ^'.join(sorted(old))}, the manifest is {version}: run {sys.argv[0]}", file=sys.stderr)
        return 1
    README.write_text(text, encoding="utf-8")
    print(f"{README.name}: ^{', ^'.join(sorted(old))} -> ^{version}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
