#!/usr/bin/env python3
"""Regression guard: Chinese text must not reach a Montserrat-only label.

Real device, 2026-11-08: Orlando @ Golden State ended in overtime (ESPN
``Final/OT``, period 5). The overview row printed "已结束" and, below it, two
placeholder boxes: the sub-line was "加时" but its label used the Latin-only
Montserrat font. Both characters do exist in app_font_cjk_16, so the glyph
coverage check alone could not catch it.

This test exercises the source-level routing check in
tools/check_font_coverage.py with good and bad variants, including the exact
indirect-assignment shape that caused the bug.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location(
    "check_font_coverage", ROOT / "tools" / "check_font_coverage.py"
)
assert _spec and _spec.loader
_check = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_check)
ui_font_routing_errors = _check.ui_font_routing_errors

CJK_FONT = 'FONT_CJK'
ASCII_FONT = '&lv_font_montserrat_14'

GOOD = f'''
static void build(void) {{
    row->status = make_label(row->card, "", {CJK_FONT}, COLOR_MUTED);
    row->status_sub = make_label(row->card, "", {CJK_FONT}, COLOR_MUTED);
    s_detail.quarter_head[4] = make_label(scr, kHead[4], {CJK_FONT}, COLOR_MUTED);
}}
static void render(bool overtime) {{
    const char *sub_text = "";
    if (overtime) sub_text = "加时";
    lv_label_set_text(row->status, "已结束");
    lv_label_set_text(row->status_sub, sub_text);
    lv_label_set_text(s_detail.quarter_head[4], "总");
}}
'''

# Exactly the shipped bug: the sub-line label was created with Montserrat.
BAD_INDIRECT = GOOD.replace(
    f'row->status_sub = make_label(row->card, "", {CJK_FONT}',
    f'row->status_sub = make_label(row->card, "", {ASCII_FONT}',
)

# A Chinese literal handed straight to a Montserrat label (no intermediate var).
BAD_DIRECT = GOOD.replace(
    f's_detail.quarter_head[4] = make_label(scr, kHead[4], {CJK_FONT}',
    f's_detail.quarter_head[4] = make_label(scr, kHead[4], {ASCII_FONT}',
)

# A Chinese literal inside make_label() itself.
BAD_LABEL_TEXT = GOOD.replace(
    's_detail.quarter_head[4] = make_label(scr, kHead[4], FONT_CJK',
    's_detail.quarter_head[4] = make_label(scr, "总", &lv_font_montserrat_14',
)


def main() -> int:
    good = ui_font_routing_errors(GOOD)
    assert good == [], f"fixed layout must pass, got: {good}"

    indirect = ui_font_routing_errors(BAD_INDIRECT)
    assert any("status_sub" in error and "sub_text" in error for error in indirect), \
        f"indirect Chinese value must be caught, got: {indirect}"

    direct = ui_font_routing_errors(BAD_DIRECT)
    assert any("quarter_head[4]" in error for error in direct), \
        f"direct Chinese literal must be caught, got: {direct}"

    label_text = ui_font_routing_errors(BAD_LABEL_TEXT)
    assert any("quarter_head[4]" in error for error in label_text), \
        f"Chinese literal in make_label() must be caught, got: {label_text}"

    print("Font routing guard: PASS (fixed / indirect / direct / make_label)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
