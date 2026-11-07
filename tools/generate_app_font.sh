#!/usr/bin/env bash
# Regenerate assets/fonts/app_font_cjk_16.c (the NBA scoreboard's Chinese subset).
#
# Why a subset and not LVGL's built-in CJK font: the built-in 16 px subset is
# missing characters this application needs (scoreboard, detail, select, ...),
# which show up as placeholder boxes on the device.
#
# Requirements:
#   * lv_font_conv (npm i -g lv_font_conv, or point LV_FONT_CONV at its entry file)
#   * Noto Sans SC Regular (SIL OFL 1.1), e.g.
#     https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf
#     The OTF itself is not committed; pass its path as the second argument.
#
# Usage:
#   ./tools/generate_app_font.sh /path/to/NotoSansSC-Regular.otf
#
# The character list must stay in sync with tools/check_font_coverage.py; run it
# after regenerating, and also run it after changing any on-screen Chinese text.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
font_file="${1:?usage: $0 /path/to/NotoSansSC-Regular.otf}"
output="${2:-${repo_root}/assets/fonts/app_font_cjk_16.c}"

# 界面实际显示的 104 个非 ASCII 字符(中文标签、状态词、间隔号),由
# tools/check_font_coverage.py 从 nba_ui.c / nba_format.c / nba_app.c 里抽取后校对。
# 末尾 7 个(手机热点打等待)来自配网页 nba_ui.c / nba_app.c。
symbols="·一上下中主份保初刷加化取回场天失始定客已建开当得总情择按换据接数断新无日时更未束板比法界留看确立第结络网节详败赛返连退选配锁键长面不创块堆大字就文最本用空绪请调足闲球员分助人到投三罚帽差犯队组手机热点打等待"

if [[ -n "${LV_FONT_CONV:-}" ]]; then
    conv=("${LV_FONT_CONV}")
elif command -v lv_font_conv >/dev/null 2>&1; then
    conv=("$(command -v lv_font_conv)")
else
    echo "lv_font_conv not found: npm i -g lv_font_conv, or set LV_FONT_CONV" >&2
    exit 1
fi

"${conv[@]}" \
    --font "${font_file}" \
    --range 0x20-0x7E \
    --symbols "${symbols}" \
    --size 16 --bpp 4 --format lvgl --no-compress \
    --lv-font-name app_font_cjk_16 --lv-include lvgl.h \
    -o "${output}"

echo "wrote ${output}"
