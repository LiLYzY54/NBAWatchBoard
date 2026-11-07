#!/usr/bin/env python3
"""Render README screen previews from the real layout constants in main/nba_ui.c.

The board has a 240x320 ST7789 panel with no framebuffer read-back (3-wire SPI,
no MISO), so our README cannot use literal device screenshots. Instead this
script paints each screen with Pillow using the exact coordinates, colors and
text from nba_ui.c, then composites it into the product render in
docs/brand/ai-passport-front.png. Sample data comes from tests/fixtures/.

These images are illustrations, NOT photographs of a running device. Keep that
note in the README whenever they are used.

Usage (from the repository root):
    python3 tools/render_screens.py

Requires Pillow and a CJK-capable system font. Nothing here is part of the
firmware build.
"""

from __future__ import annotations

import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "screens"
DEVICE = ROOT / "docs" / "brand" / "ai-passport-front.png"
FIXTURES = ROOT / "tests" / "fixtures"

W, H = 240, 320
S = 3  # supersampling for crisp text
SCREEN_AREA = (126, 142, 408, 518)  # blue UI rectangle in the product render

# main/nba_ui.c
BG = "#070A10"
CARD = "#121A25"
CARD_SEL = "#1B2C46"
STROKE = "#243244"
TEXT = "#F2F6FB"
MUTED = "#8FA0B5"
LIVE = "#FF5A4D"
BATTERY = "#6BE07A"
DAY_AWAY = "#F2A03B"

TEAM_COLORS = {
    "ATL": "#E03A3E", "BOS": "#00A758", "BKN": "#C9CDD4", "CHA": "#2C5697",
    "CHI": "#CE1141", "CLE": "#FDBB30", "DAL": "#3B7BC4", "DEN": "#EDB53A",
    "DET": "#C8102E", "GS": "#2F7DE1", "GSW": "#2F7DE1", "HOU": "#CE1141",
    "IND": "#FDBB30", "LAC": "#C8102E", "LAL": "#FDB927", "MEM": "#5D76A9",
    "MIA": "#E12A43", "MIL": "#3C8C6E", "MIN": "#7EC7E8", "NO": "#8C734B",
    "NOP": "#8C734B", "NY": "#F58427", "NYK": "#F58427", "OKC": "#3A8FD4",
    "ORL": "#3C77C7", "PHI": "#2C6BB3", "PHX": "#E56020", "POR": "#D6393F",
    "SAC": "#8A6BD1", "SA": "#B8C4CC", "SAS": "#B8C4CC", "TOR": "#D7263D",
    "UTA": "#F7A22B", "UTAH": "#F7A22B", "WSH": "#E31837", "WAS": "#E31837",
}

FONT_CANDIDATES = [
    ("/System/Library/Fonts/Supplemental/Arial.ttf", False, False),
    ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", True, False),
    ("/Library/Fonts/Arial.ttf", False, False),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", False, False),
    ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", True, False),
]
CJK_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/System/Library/Fonts/PingFang.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
]


def _font(bold: bool, cjk: bool, px: int) -> ImageFont.FreeTypeFont:
    candidates = CJK_CANDIDATES if cjk else [p for p, b, _ in FONT_CANDIDATES if b == bold]
    if not cjk and not candidates:
        candidates = [p for p, _, _ in FONT_CANDIDATES]
    for path in candidates:
        if Path(path).exists():
            try:
                return ImageFont.truetype(path, px)
            except OSError:
                continue
    return ImageFont.load_default(px)


class Screen:
    def __init__(self) -> None:
        self.im = Image.new("RGB", (W * S, H * S), BG)
        self.d = ImageDraw.Draw(self.im)

    def rect(self, x, y, w, h, r, fill, outline=None, width=1):
        box = [x * S, y * S, (x + w) * S - 1, (y + h) * S - 1]
        self.d.rounded_rectangle(box, radius=r * S, fill=fill,
                                 outline=outline, width=width * S)

    def text(self, x, y, s, size=14, color=TEXT, w=0, align="left",
             bold=False, cjk=False):
        if not s:
            return
        cjk = cjk or any(ord(ch) > 0x2E80 for ch in s)
        f = _font(bold, cjk, size * S)
        tw = self.d.textlength(s, font=f)
        if w:
            if align == "right":
                tx = (x + w) * S - tw
            elif align == "center":
                tx = x * S + (w * S - tw) / 2
            else:
                tx = x * S
        else:
            tx = x * S
        top = self.d.textbbox((0, 0), s, font=f)[1]
        self.d.text((tx, y * S - top), s, font=f, fill=color)

    def battery(self, soc=85):
        self.rect(196, 12, 26, 12, 3, CARD, outline=STROKE)
        self.rect(197, 13, 18 * soc // 100, 10, 2, BATTERY)
        self.rect(222, 15, 2, 6, 1, STROKE)
        self.text(168, 10, str(soc), 14, MUTED, w=26, align="right")

    def finish(self) -> Image.Image:
        # The physical panel has a 30px corner radius; mask offscreen corners.
        mask = Image.new("L", (W * S, H * S), 0)
        ImageDraw.Draw(mask).rounded_rectangle(
            [0, 0, W * S - 1, H * S - 1], radius=30 * S, fill=255)
        out = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
        out.paste(self.im, (0, 0), mask)
        return out


def header(scr: Screen, net="已连接"):
    scr.text(12, 10, "NBA 看板", 16, TEXT, cjk=True)
    scr.text(84, 12, "10-31", 14, MUTED, w=80, align="right")
    scr.text(12, 32, net, 16, MUTED, cjk=True)
    scr.battery()


def overview() -> Screen:
    scr = Screen()
    header(scr)
    games = [
        ("CHA", "79", "WSH", "95", "已结束", "", "post"),
        ("CHI", "111", "BOS", "125", "已结束", "", "post"),
        ("DET", "110", "BKN", "94", "已结束", "", "post"),
        ("OKC", "108", "LAL", "102", "第 3 节", "05:42", "live"),
    ]
    for i, (away, ascore, home, hscore, status, sub, state) in enumerate(games):
        y = 54 + i * 58
        selected = i == 0
        scr.rect(8, y, 224, 54, 10, CARD_SEL if selected else CARD,
                 outline=LIVE if selected else None)
        accent = LIVE if state == "live" else TEAM_COLORS.get(home, "#5A6B80")
        scr.rect(8, y + 8, 4, 38, 2, accent)
        scr.text(22, y + 2, away, 20, TEAM_COLORS.get(away, MUTED),
                 w=60, bold=True)
        scr.text(84, y, ascore, 28, TEXT, w=60, align="right", bold=True)
        scr.text(22, y + 26, home, 20, TEAM_COLORS.get(home, MUTED),
                 w=60, bold=True)
        scr.text(84, y + 24, hscore, 28, TEXT, w=60, align="right", bold=True)
        scr.text(150, y + 6, status, 16, LIVE if state == "live" else MUTED,
                 w=74, align="right", cjk=True)
        scr.text(150, y + 28, sub, 14, MUTED, w=74, align="right")
    scr.text(4, 296, "长按确定刷新·长按上下换日", 16, MUTED, w=232,
             align="center", cjk=True)
    return scr


def detail() -> Screen:
    scr = Screen()
    scr.text(10, 10, "已结束", 16, TEXT, cjk=True)
    scr.text(10, 32, "已连接", 16, MUTED, cjk=True)
    scr.text(10, 266, "比赛日 10-31", 16, MUTED, cjk=True)
    scr.battery()
    scr.text(12, 58, "客", 16, MUTED, cjk=True)
    scr.text(130, 58, "主", 16, MUTED, cjk=True)
    scr.text(12, 78, "CHI", 20, TEAM_COLORS["CHI"], bold=True)
    scr.text(130, 78, "BOS", 20, TEAM_COLORS["BOS"], bold=True)
    scr.text(12, 100, "111", 40, TEXT, bold=True)
    scr.text(130, 100, "125", 40, TEXT, bold=True)
    scr.rect(116, 100, 2, 32, 1, STROKE)
    scr.text(12, 146, "1-5", 14, MUTED)
    scr.text(130, 146, "4-1", 14, MUTED)
    heads = [("Q1", 48, 28, "center"), ("Q2", 80, 28, "center"),
             ("Q3", 112, 28, "center"), ("Q4", 144, 28, "center"),
             ("总", 174, 40, "right")]
    for label, x, w, align in heads:
        scr.text(x, 178, label, 14, MUTED, w=w, align=align, cjk=(label == "总"))
    away_q = ["25", "24", "25", "37", "111"]
    home_q = ["44", "34", "21", "26", "125"]
    for i, (label, x, w, align) in enumerate(heads):
        scr.text(x, 198, away_q[i], 20, TEXT, w=w, align=align, bold=True)
        scr.text(x, 228, home_q[i], 20, TEXT, w=w, align=align, bold=True)
    scr.text(4, 296, "上下换场·确定球员·长按返回", 16, MUTED, w=232,
             align="center", cjk=True)
    return scr


def _players():
    box = json.loads((FIXTURES / "nba_summary_box.json").read_text())
    group = box["boxscore"]["players"][0]["statistics"][0]
    keys = group["keys"]
    rows = []
    for a in group["athletes"]:
        st = dict(zip(keys, a["stats"]))
        rows.append((a["athlete"]["shortName"], st))
    return rows


def box_page(group_idx: int) -> Screen:
    scr = Screen()
    scr.text(10, 8, "球员数据", 16, TEXT, cjk=True)
    scr.text(76, 10, "10-31", 14, MUTED, w=88, align="right")
    scr.battery()
    scr.text(10, 30, "POR 121 - 110 BKN  已结束", 14, MUTED, cjk=True)
    scr.text(10, 56, "主队", 16, MUTED, cjk=True)
    scr.text(46, 50, "POR", 20, TEXT, bold=True)
    scr.text(168, 50, "121", 20, TEXT, w=62, align="right", bold=True)
    scr.text(10, 78, "球员", 16, MUTED, cjk=True)
    heads = (("分", "板", "助"), ("投", "三", "罚"), ("断", "帽", "失"),
             ("时", "差", "犯"))[group_idx]
    for c, head in enumerate(heads):
        scr.text(132 + c * 36, 78, head, 16, MUTED, w=32, align="right", cjk=True)

    players = _players()
    fields = (
        ("points", "rebounds", "assists"),
        ("fieldGoalsMade-fieldGoalsAttempted",
         "threePointFieldGoalsMade-threePointFieldGoalsAttempted",
         "freeThrowsMade-freeThrowsAttempted"),
        ("steals", "blocks", "turnovers"),
        ("minutes", "plusMinus", "fouls"),
    )[group_idx]
    for i, (name, st) in enumerate(players[:8]):
        y = 96 + i * 22
        scr.rect(4, y, 236, 20, 6, CARD_SEL)
        name_size = 12 if len(name) > 17 else 14
        scr.text(8, y + 2, name, name_size, TEXT, w=124)
        for c, key in enumerate(fields):
            value = st.get(key, "")
            size = 14
            if len(value) >= 5:      # "10-13": the two-digit/two-digit case
                size = 12
            scr.text(132 + c * 36, y + 2, value, size, TEXT, w=32, align="right")
    scr.text(4, 278, "上下选人·确定换组", 16, MUTED, w=232, align="center", cjk=True)
    scr.text(4, 298, "长按上下换队·长按确定返回", 16, MUTED, w=232,
             align="center", cjk=True)
    return scr


def provisioning() -> Screen:
    scr = Screen()
    scr.text(12, 12, "Wi-Fi 配网", 16, TEXT, cjk=True)
    scr.rect(8, 52, 224, 172, 12, CARD)
    scr.rect(8, 66, 4, 144, 2, LIVE)
    scr.text(26, 68, "手机连热点", 16, MUTED, cjk=True)
    scr.text(26, 92, "NBA-Board", 20, TEXT, bold=True)
    scr.text(26, 136, "打开", 16, MUTED, cjk=True)
    scr.text(26, 160, "192.168.4.1", 20, DAY_AWAY, bold=True)
    scr.text(12, 240, "等待连接", 16, MUTED, cjk=True)
    scr.text(4, 296, "长按确定返回", 16, MUTED, w=232, align="center", cjk=True)
    return scr


def composite(screen: Image.Image) -> Image.Image:
    base = Image.open(DEVICE).convert("RGBA")
    x1, y1, x2, y2 = SCREEN_AREA
    # The product render shows the factory demo UI. Wipe that rectangle first,
    # otherwise its blue rounded corners peek out from under our panel.
    cover = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(cover).rectangle([x1 - 2, y1 - 2, x2 + 2, y2 + 2],
                                    fill=(7, 10, 16, 255))
    base.alpha_composite(cover)
    panel = screen.resize((x2 - x1, y2 - y1), Image.LANCZOS)
    mask = Image.new("L", panel.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, panel.width - 1, panel.height - 1], radius=20, fill=255)
    base.paste(panel, (x1, y1), mask)
    return base.convert("RGB")


GRID_LABELS = (
    "Overview - four games",
    "Detail - quarters and total",
    "Players - PTS / REB / AST",
    "Players - FG / 3PT / FT",
    "Players - MIN / +/- / PF",
    "First-time Wi-Fi setup",
)


def grid(images) -> Image.Image:
    cell_w, cell_h, pad, top = 240, 369, 18, 54
    cols = 3
    rows = (len(images) + cols - 1) // cols
    canvas = Image.new("RGB", (cols * cell_w + (cols + 1) * pad,
                               rows * (cell_h + 30) + (rows + 1) * pad + top),
                       "#0B1018")
    d = ImageDraw.Draw(canvas)
    d.text((pad, 14), "NBA WatchBoard - screen previews",
           font=_font(True, False, 24), fill="#F2F6FB")
    for i, im in enumerate(images):
        c, r = i % cols, i // cols
        x = pad + c * (cell_w + pad)
        y = top + pad + r * (cell_h + 30 + pad)
        canvas.paste(im.resize((cell_w, cell_h), Image.LANCZOS), (x, y))
        d.text((x, y + cell_h + 6), GRID_LABELS[i],
               font=_font(False, False, 15), fill="#8FA0B5")
    return canvas


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    pages = [
        ("01-overview", "总览 · 4 场比赛", overview()),
        ("02-detail", "详情 · 节次与总分", detail()),
        ("03-box-points", "球员 · 分/板/助", box_page(0)),
        ("04-box-shooting", "球员 · 投/三/罚", box_page(1)),
        ("05-box-misc", "球员 · 时/差/犯", box_page(3)),
        ("06-provisioning", "首次配网", provisioning()),
    ]
    composed = []
    for stem, label, scr in pages:
        raw = scr.finish()
        raw.convert("RGB").save(OUT / f"{stem}-screen.png")
        shot = composite(raw)
        shot.save(OUT / f"{stem}.png")
        composed.append(shot)
        print(f"  {stem}.png  ({label})")
    grid(composed).save(OUT / "screens-grid.png")
    print(f"  screens-grid.png\nWrote {len(composed) * 2 + 1} images to {OUT}")


if __name__ == "__main__":
    main()
