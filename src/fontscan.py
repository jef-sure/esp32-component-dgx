"""
 * MIT License
 *
 * Copyright (c) 2021 Anton Petrusevich
 *
"""
import glob
import os
import re

SCRIPT_DIR = os.path.dirname(os.path.realpath(__file__))
FONTS_DIR = os.path.join(SCRIPT_DIR, "fonts")
FONTS_TXT_PATH = os.path.join(FONTS_DIR, "fonts.txt")
PROJECT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "../../.."))
SOURCE_FILE_RE = re.compile(r"\.(c|cc|cpp|cxx)$", re.IGNORECASE)


def read_text_if_exists(path):
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as file_handle:
            return file_handle.read()
    except FileNotFoundError:
        return None


def write_if_changed(path, content):
    existing_content = read_text_if_exists(path)
    if existing_content != content:
        with open(path, "w", encoding="utf-8") as file_handle:
            file_handle.write(content)


all_fonts = [
    font_path[len(SCRIPT_DIR) + 1:]
    for font_path in glob.glob(os.path.join(FONTS_DIR, "*.c"))
]
all_headers = [re.sub(r"\.c$", ".h", font_path) for font_path in all_fonts]

used = set()
for current_dir, _subdirs, files in os.walk(PROJECT_ROOT):
    _subdirs[:] = [subdir for subdir in _subdirs if subdir != "build" and not subdir.startswith(".")]
    for file_name in files:
        if SOURCE_FILE_RE.search(file_name) is None:
            continue
        source_path = os.path.join(current_dir, file_name)
        with open(source_path, "r", encoding="utf-8", errors="ignore") as file_handle:
            content = file_handle.read()
        for header in all_headers:
            if re.search(r"^#include\s*\"" + re.escape(header) + r"\"", content, re.MULTILINE) is not None:
                used.add(re.sub(r"^fonts/", "", re.sub(r"\.h$", ".c", header)))

fonts_txt = "\n".join("src/fonts/" + font_name for font_name in sorted(used))
write_if_changed(FONTS_TXT_PATH, fonts_txt)

