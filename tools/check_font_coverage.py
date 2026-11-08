#!/usr/bin/env python3
"""Verify every non-ASCII character the NBA board draws exists in its LVGL font.

The board renders Chinese with its own generated subset font
(``assets/fonts/app_font_cjk_16.c``, Noto Sans SC 16 px). A UTF-8 string compiles fine even
when the font has no glyph for it, and on screen that shows up as a placeholder
box. This check turns that into a build-time failure:

* it reads the string literals of the files that produce on-screen text
  (``main/nba_ui.c`` for labels, ``main/nba_format.c`` for formatted values,
  ``main/nba_app.c`` for the header status line);
* it reads the glyph coverage of that generated font source;
* it reports every character that has no glyph;
* it checks that Chinese text is not written into a Montserrat-only label
  (a glyph can exist in the font yet still be drawn as a box if the label
  uses the wrong font — see ``ui_font_routing_errors``).

Run from the repository root: ``python3 tools/check_font_coverage.py``.
If the font source is missing the check fails instead of pretending success.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SOURCES = ("main/nba_ui.c", "main/nba_format.c", "main/nba_app.c")
FONT_NAME = "app_font_cjk_16"
FONT_PATH = Path("assets/fonts/app_font_cjk_16.c")


def drop_log_lines(text: str) -> str:
    """把 ``ESP_LOGx(...)`` 所在的整行抹掉。

    日志走串口,不经过 LVGL 字体。若把它们也纳入检查,一句日志多写几个字就会
    报"缺字形",把人逼去往字体里塞永远不上屏的字;而真正该守的是屏幕上那些
    文案。本项目日志一律单行(不跨行拼接),按行剔除即可。
    """
    return "".join(
        "\n" if "ESP_LOG" in line else line for line in text.splitlines(keepends=True)
    )



def strip_comments_and_literals(text: str) -> list[str]:
    """Return the C string literals in ``text``, ignoring comments."""
    literals: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        two = text[i : i + 2]
        if two == "//":
            while i < n and text[i] != "\n":
                i += 1
        elif two == "/*":
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
        elif text[i] == '"':
            i += 1
            chunk: list[str] = []
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    esc = text[i + 1]
                    if esc == "u" and re.fullmatch(r"[0-9a-fA-F]{4}", text[i + 2 : i + 6]):
                        chunk.append(chr(int(text[i + 2 : i + 6], 16)))
                        i += 6
                        continue
                    mapping = {"n": "\n", "t": "\t", "r": "\r", "0": "\0",
                               "\\": "\\", '"': '"', "'": "'"}
                    chunk.append(mapping.get(esc, esc))
                    i += 2
                    continue
                chunk.append(text[i])
                i += 1
            literals.append("".join(chunk))
            i += 1
        elif text[i] == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        else:
            i += 1
    return literals


def cmap_coverage(font_source: str) -> set[int]:
    """Parse the glyph table of a generated LVGL font into a code point set.

    LVGL 9 fonts list one ``cmaps`` entry per continuous range. Ranges whose
    ``unicode_list`` is NULL cover every code point in the range; sparse ranges
    (``unicode_list_N``) cover only the offsets stored in that array.
    """
    offsets: dict[str, list[int]] = {}
    for match in re.finditer(
        r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", font_source, re.S
    ):
        offsets[match.group(1)] = [
            int(value, 0) for value in re.findall(r"0x[0-9a-fA-F]+|\d+", match.group(2))
        ]

    covered: set[int] = set()
    for match in re.finditer(
        r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = \d+,(.*?)\}",
        font_source,
        re.S,
    ):
        start = int(match.group(1))
        length = int(match.group(2))
        body = match.group(3)
        if ".unicode_list = NULL" in body:
            covered.update(range(start, start + length))
            continue
        name = re.search(r"\.unicode_list = (\w+)", body)
        if name:
            covered.update(start + offset for offset in offsets.get(name.group(1), []))

    return covered


# A label is a "Latin-only label" when make_label() gave it a Montserrat font.
# app_font_cjk_16 also covers 0x20-0x7E, so labels using it can show both.
_LABEL_ASSIGN = re.compile(
    r"([A-Za-z_][A-Za-z0-9_.\[\]>-]*)\s*=\s*make_label\s*\((.*?)\);", re.S
)
_CJK_ASSIGN = re.compile(r"\b([A-Za-z_]\w*)\s*=\s*\"([^\"]*)\"")
_SET_TEXT = re.compile(
    r"lv_label_set_text\s*\(\s*([A-Za-z_][A-Za-z0-9_.\[\]>-]*)\s*,\s*(.*?)\)\s*;", re.S
)


def _has_cjk(text: str) -> bool:
    return any(ord(char) > 0x7F for char in text)


def ui_font_routing_errors(ui_source: str) -> list[str]:
    """Find Chinese text handed to a label whose font has no Chinese glyphs.

    Glyph coverage is not enough: ``"加时"`` is present in app_font_cjk_16, but
    a label created with ``&lv_font_montserrat_14`` renders each character as a
    placeholder box.  On 2026-11-08, Orlando @ Golden State finished in
    overtime, so the overview row showed "已结束" with exactly two boxes below.

    This is a lightweight source-level data-flow check: track label variables
    created with Montserrat, track variables assigned Chinese string literals,
    then reject either kind of Chinese value being passed to lv_label_set_text
    on those labels.  It is deliberately scoped to nba_ui.c's direct calls;
    values coming from outside the file (network text, formatted buffers) are
    the caller's responsibility and are covered by the label's font choice.
    """
    errors: list[str] = []
    ascii_labels: set[str] = set()
    cjk_vars: set[str] = set()

    for match in _LABEL_ASSIGN.finditer(ui_source):
        name, body = match.group(1), match.group(2)
        if "lv_font_montserrat" in body:
            ascii_labels.add(name)
            for literal in strip_comments_and_literals(body):
                if _has_cjk(literal):
                    errors.append(f"{name}: Chinese literal {literal!r} uses a Montserrat font")

    for match in _CJK_ASSIGN.finditer(ui_source):
        if _has_cjk(match.group(2)):
            cjk_vars.add(match.group(1))

    for match in _SET_TEXT.finditer(ui_source):
        label, value = match.group(1).strip(), match.group(2).strip()
        if label not in ascii_labels:
            continue
        if value in cjk_vars:
            errors.append(f"{label}: Chinese text via {value!r} uses a Montserrat font")
        elif value.startswith('"') and _has_cjk(value):
            errors.append(f"{label}: Chinese literal {value!r} uses a Montserrat font")
    return errors


def main() -> int:
    font_path = FONT_PATH
    if not font_path.is_file():
        print(f"FAIL font coverage: {font_path} not present")
        return 1

    covered = cmap_coverage(font_path.read_text(encoding="utf-8"))
    if not covered:
        print(f"FAIL font coverage: no glyph table parsed from {font_path}")
        return 1

    required: dict[int, str] = {}
    for source in SOURCES:
        path = Path(source)
        if not path.is_file():
            print(f"FAIL font coverage: missing {source}")
            return 1
        for literal in strip_comments_and_literals(
            drop_log_lines(path.read_text(encoding="utf-8"))
        ):
            for char in literal:
                if ord(char) > 0x7F:
                    required.setdefault(ord(char), source)

    missing = sorted(cp for cp in required if cp not in covered)
    if missing:
        print(f"FAIL font coverage: {len(missing)} character(s) have no glyph in {FONT_NAME}")
        for cp in missing:
            print(f"  U+{cp:04X} {chr(cp)!r} (first seen in {required[cp]})")
        return 1

    ui_path = Path("main/nba_ui.c")
    routing = ui_font_routing_errors(ui_path.read_text(encoding="utf-8")) \
        if ui_path.is_file() else []
    if routing:
        print("FAIL font routing: Chinese text on a Montserrat-only label")
        for error in routing:
            print(f"  {error}")
        return 1

    print(f"Font coverage: PASS ({len(required)} non-ASCII code points, "
          f"{len(covered)} covered by {FONT_NAME}; UI font routing: PASS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
