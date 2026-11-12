#!/usr/bin/env python3
"""Build 3:4 community artwork for the NBA WatchBoard play (v3).

v3 fixes the layout problems reported after v2:
* every pill is sized from its own text instead of a hard-coded box;
* every text inside a card is fitted to the card's inner width;
* the "real UI" badge no longer sits on top of the device render;
* the CTA / footer / feature-list line spacing is opened up.

A Layout object records the exact bounding box of each text run and verifies
that it stays inside its container, so a regression fails loudly.

Run from anywhere: ``python3 tools/render_community_assets.py``.
Requires Pillow, segno and the macOS system fonts listed below; the artwork is
generated on macOS and committed, so CI does not need to render it.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

try:
    import segno
except ImportError:  # optional local copy used while authoring the artwork
    sys.path.insert(0, "/tmp/fap_qrlib")
    import segno  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "docs/community"
FRONT = ROOT / "docs/brand/ai-passport-front.png"
SCREENS = ROOT / "docs/screens"
REPO = "https://github.com/LiLYzY54/NBAWatchBoard"
REPO_TEXT = "github.com/LiLYzY54/NBAWatchBoard"

W, H = 1152, 1536
M = 72                      # page margin
RED = (255, 90, 77)
AMBER = (242, 160, 59)
TEXT = (242, 246, 251)
MUTED = (150, 166, 188)
DIM = (105, 120, 142)

HIRA = "/System/Library/Fonts/Hiragino Sans GB.ttc"
STHEITI = "/System/Library/Fonts/STHeiti Medium.ttc"
ARIAL_B = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
ARIAL = "/System/Library/Fonts/Supplemental/Arial.ttf"

_cache: dict[tuple, ImageFont.FreeTypeFont] = {}


def font(size, bold=False, latin=False):
    key = (size, bold, latin)
    if key in _cache:
        return _cache[key]
    if latin:
        candidates = [(ARIAL_B if bold else ARIAL, 0), (HIRA, 1 if bold else 0)]
    else:
        candidates = [(HIRA, 1 if bold else 0), (STHEITI, 0), (ARIAL, 0)]
    for path, index in candidates:
        if Path(path).exists():
            try:
                f = ImageFont.truetype(path, size, index=index)
                _cache[key] = f
                return f
            except OSError:
                continue
    f = ImageFont.load_default(size)
    _cache[key] = f
    return f


def rgba(c):
    return tuple(c) + (255,) if len(c) == 3 else tuple(c)


class Layout:
    """Draw text and assert that it never leaves its container."""

    ALL: list["Layout"] = []

    def __init__(self, d):
        self.d = d
        self.issues: list[str] = []
        self.count = 0
        Layout.ALL.append(self)

    def measure(self, s, size, bold=False, spacing=0, latin=False):
        f = font(size, bold, latin)
        return sum(self.d.textlength(c, font=f) for c in s) + spacing * max(0, len(s) - 1)

    def fit(self, s, size, max_w, bold=False, spacing=0, latin=False, min_size=18):
        sz = size
        while sz > min_size and self.measure(s, sz, bold, spacing, latin) > max_w:
            sz -= 1
        return sz

    def check(self, s, x, y, w, bbox, container, pad=0):
        """bbox is d.textbbox at origin; container is (x0,y0,x1,y1)."""
        if container is None:
            return
        x0, y0, x1, y1 = container
        top, bot = y + bbox[1], y + bbox[3]
        bad = []
        if x < x0 + pad - 1:
            bad.append(f"left {x:.0f} < {x0 + pad:.0f}")
        if x + w > x1 - pad + 1:
            bad.append(f"right {x + w:.0f} > {x1 - pad:.0f}")
        if top < y0 + pad - 1:
            bad.append(f"top {top:.0f} < {y0 + pad:.0f}")
        if bot > y1 - pad + 1:
            bad.append(f"bottom {bot:.0f} > {y1 - pad:.0f}")
        if bad:
            self.issues.append(f"{s!r} OUTSIDE {container}: " + ", ".join(bad))
        self.count += 1

    def text(self, x, y, s, size, color=TEXT, bold=False, spacing=0, latin=False,
             max_w=None, container=None, pad=0, min_size=18, tag=None):
        sz = self.fit(s, size, max_w, bold, spacing, latin, min_size) if max_w else size
        f = font(sz, bold, latin)
        fill = rgba(color)
        cx = x
        for ch in s:
            self.d.text((cx, y), ch, font=f, fill=fill)
            cx += self.d.textlength(ch, font=f) + spacing
        w = self.measure(s, sz, bold, spacing, latin)
        bbox = self.d.textbbox((0, 0), "Hg国", font=f)
        self.check(tag or s, x, y, w, bbox, container, pad)
        return cx - spacing

    def pill(self, x, y, label, fg=TEXT, bg=(255, 255, 255, 14), size=24,
             outline=(255, 255, 255, 34), h=52, pad=26, right=None, max_w=None):
        """Auto-width pill. Returns (left, right, bottom)."""
        sz = self.fit(label, size, max_w - 2 * pad) if max_w else size
        f = font(sz)
        tw = self.d.textlength(label, font=f)
        w = int(tw + 2 * pad)
        left = x if right is None else right - w
        box = (left, y, left + w, y + h)
        self.d.rounded_rectangle(box, radius=h // 2, fill=bg, outline=outline, width=2)
        bbox = self.d.textbbox((0, 0), label, font=f)
        th = bbox[3] - bbox[1]
        ty = y + (h - th) / 2 - bbox[1]
        self.d.text((left + pad, ty), label, font=f, fill=rgba(fg))
        self.check(label, left + pad, ty, tw, bbox, box, pad=8)
        return left, left + w, y + h


def background() -> Image.Image:
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    top, bot = (12, 17, 27), (5, 8, 14)
    for y in range(H):
        t = y / H
        d.line([(0, y), (W, y)],
               fill=tuple(int(top[i] + (bot[i] - top[i]) * t) for i in range(3)))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    gd.ellipse((-300, -520, 700, 360), fill=(255, 90, 77, 74))
    gd.ellipse((760, 980, 1560, 1760), fill=(242, 160, 59, 34))
    gd.ellipse((820, -260, 1500, 300), fill=(60, 110, 220, 26))
    im = Image.alpha_composite(im.convert("RGBA"),
                               glow.filter(ImageFilter.GaussianBlur(170)))
    deco = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    dd = ImageDraw.Draw(deco)
    for x in range(0, W, 72):
        dd.line([(x, 0), (x, H)], fill=(255, 255, 255, 7), width=1)
    for y in range(0, H, 72):
        dd.line([(0, y), (W, y)], fill=(255, 255, 255, 7), width=1)
    dd.arc((-180, 940, 1330, 2100), 180, 360, fill=(255, 255, 255, 20), width=3)
    dd.arc((-20, 1060, 1170, 2060), 180, 360, fill=(255, 255, 255, 12), width=2)
    return Image.alpha_composite(im, deco)


class Canvas:
    def __init__(self):
        self.im = background()
        self._reset()

    def _reset(self):
        self.ov = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.ov)

    def flush(self):
        self.im = Image.alpha_composite(self.im, self.ov)
        self._reset()

    def paste_rgba(self, obj, x, y, shadow=True, blur=40, alpha=160, offset=24):
        self.flush()
        if shadow:
            sh = Image.new("RGBA", obj.size, (0, 0, 0, 0))
            sh.putalpha(obj.getchannel("A").point(lambda v: v * alpha // 255))
            self.im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)),
                                    (x + offset // 2, y + offset))
        self.im.alpha_composite(obj, (x, y))

    def paste_rgb(self, obj, x, y):
        self.flush()
        self.im.paste(obj, (x, y))

    def finish(self):
        self.flush()
        return self.im.convert("RGB")


def device(screen_path) -> Image.Image:
    base = Image.open(FRONT).convert("RGBA")
    scr = Image.open(screen_path).convert("RGBA").resize((282, 376), Image.LANCZOS)
    mask = Image.new("L", (282, 376), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 281, 375), radius=20, fill=255)
    base.paste(scr, (126, 142), mask)
    return base


def screen_card(screen_path, w=600) -> Image.Image:
    src = Image.open(screen_path).convert("RGBA")
    h = int(src.height * w / src.width)
    im = src.resize((w, h), Image.LANCZOS)
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, w - 1, h - 1),
                                           radius=int(w * 0.085), fill=255)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    out.paste(im, (0, 0), mask)
    ImageDraw.Draw(out).rounded_rectangle(
        (1, 1, w - 2, h - 2), radius=int(w * 0.085),
        outline=(255, 255, 255, 70), width=3)
    return out


def footer(L, y=1476, line_y=1440):
    L.d.line([(M, line_y), (W - M, line_y)], fill=(255, 255, 255, 22), width=2)
    band = (M - 4, line_y + 8, W - M + 4, H - 12)
    L.text(M, y, "界面示意图 · 非实机照片", 22, DIM,
           container=band, pad=2)
    f = font(24)
    tw = L.d.textlength(REPO_TEXT, font=f)
    L.text(W - M - tw, y, REPO_TEXT, 24, MUTED, latin=True,
           container=band, pad=2)


def small_header(L, idx, tag):
    _, right, _ = L.pill(M, 74, idx, fg=AMBER, bg=(242, 160, 59, 26), size=26,
                         outline=(242, 160, 59, 90), h=48, pad=26)
    L.text(right + 28, 84, tag, 26, MUTED, bold=True, spacing=2,
           max_w=W - M - (right + 28) - 28)


# ------------------------------------------------------------------ cover
def cover(c):
    L = Layout(c.d)
    L.pill(M, 76, "FOLOTOY · AI PASSPORT 玩法", fg=MUTED,
           bg=(255, 255, 255, 12), size=22, outline=(255, 255, 255, 30), h=52)
    L.pill(0, 76, "开源 · 免费下载", fg=AMBER, bg=(242, 160, 59, 24), size=22,
           outline=(242, 160, 59, 80), h=52, right=W - M)

    # headline — generous leading between the two lines and the copy below
    y = 178
    x = L.text(M, y, "把整个 ", 88, TEXT, bold=True, spacing=2,
               max_w=W - 2 * M)
    L.text(x, y - 4, "NBA", 94, RED, bold=True, spacing=2, latin=True,
           max_w=W - M - x)
    L.text(M, y + 128, "装进口袋", 88, TEXT, bold=True, spacing=4,
           max_w=W - 2 * M)
    L.d.rounded_rectangle((74, y + 254, 200, y + 262), radius=4, fill=rgba(RED))
    # Keep the copy in the left column only: the right column belongs to the
    # device render, so long lines must never run underneath it.
    L.text(M, y + 286, "一个比赛日，全部场次与比分，抬手就看。", 30, MUTED,
           max_w=520)
    L.text(M, y + 340, "比分自己更新，长按还能翻前后几天。", 30, MUTED,
           max_w=520)

    cards = [
        ("01", "全天 16 场一屏排开", "上下翻场，队名 / 比分 / 节次一眼看全"),
        ("02", "比分自己会更新", "激战时刷得勤，全部结束就安静下来"),
        ("03", "点开就是球员数据", "分节比分、总分，四组技术统计一键切换"),
    ]
    card_box_x0, card_box_x1 = M, 568
    cy, card_h, step = 622, 186, 208
    for num, title, sub in cards:
        box = (card_box_x0, cy, card_box_x1, cy + card_h)
        L.d.rounded_rectangle(box, radius=22,
                              fill=(255, 255, 255, 11), outline=(255, 255, 255, 30), width=2)
        L.d.rounded_rectangle((card_box_x0, cy + 26, card_box_x0 + 6, cy + card_h - 26),
                              radius=3, fill=rgba(RED))
        inner = (100, cy + 12, card_box_x1 - 20, cy + card_h - 12)
        L.text(104, cy + 16, num, 28, AMBER, bold=True, latin=True,
               container=inner, pad=2)
        L.text(104, cy + 60, title, 36, TEXT, bold=True,
               max_w=card_box_x1 - 104 - 28, container=inner, pad=2)
        L.text(104, cy + 118, sub, 25, MUTED,
               max_w=card_box_x1 - 104 - 28, container=inner, pad=2, min_size=20)
        cy += step

    # device render: fully inside the right margin, and its bottom leaves a
    # clear gap before the badge and the CTA bar
    dev = device(SCREENS / "01-overview-screen.png")
    dw = 470
    dev = dev.resize((dw, int(dev.height * dw / dev.width)), Image.LANCZOS)
    c.paste_rgba(dev, 610, 566, blur=52, alpha=175, offset=30)
    L = Layout(c.d)
    L.pill(0, 1304, "真 · 运行画面", fg=TEXT, bg=(23, 14, 19, 238), size=26,
           outline=(255, 90, 77, 150), h=56, right=1050)

    cta = (M, 1378, W - M, 1492)
    L.d.rounded_rectangle(cta, radius=20,
                          fill=(255, 255, 255, 12), outline=(255, 255, 255, 34), width=2)
    L.text(104, 1394, "★", 38, AMBER, container=cta, pad=16)
    L.text(166, 1400, "源码开源 · 顺手点个 Star", 32, TEXT, bold=True,
           max_w=620, container=cta, pad=16)
    L.text(166, 1450, REPO_TEXT, 24, MUTED, latin=True,
           container=cta, pad=16)
    s = "仓库二维码见第 4 图 →"
    w = L.measure(s, 24)
    L.text(W - M - 28 - w, 1410, s, 24, MUTED, container=cta, pad=16)
    return L


# --------------------------------------------------------------- galleries
def gallery(c, idx, tag, t1, t2, subtitle, screen, bullets):
    L = Layout(c.d)
    small_header(L, idx, tag)
    L.text(M, 154, t1, 74, TEXT, bold=True, spacing=2, max_w=W - 2 * M)
    L.text(M, 258, t2, 74, TEXT, bold=True, spacing=2, max_w=W - 2 * M)
    L.d.rounded_rectangle((74, 372, 182, 379), radius=4, fill=rgba(RED))
    L.text(M, 404, subtitle, 30, MUTED, max_w=W - 2 * M)

    card = screen_card(screen, 600)
    cx, cy = (W - card.width) // 2, 482
    c.paste_rgba(card, cx, cy, blur=46, alpha=165, offset=26)
    L = Layout(c.d)
    by, bx = cy + card.height + 36, M
    for b in bullets:
        _, right, _ = L.pill(bx, by, b, bg=(255, 255, 255, 12), size=26,
                             outline=(255, 255, 255, 34), h=56,
                             max_w=W - 2 * M)
        bx = right + 18
    footer(L)
    return L


# ---------------------------------------------------------------- qr page
def qr_page(c):
    L = Layout(c.d)
    small_header(L, "04", "开源 · 进仓库看看")
    L.text(M, 154, "喜欢这个看板？", 74, TEXT, bold=True, spacing=2,
           max_w=W - 2 * M)
    x = L.text(M, 258, "顺手点个 ", 74, TEXT, bold=True, spacing=2,
               max_w=W - 2 * M)
    L.text(x, 254, "Star", 80, AMBER, bold=True, latin=True,
           max_w=W - M - x)
    L.text(M, 396, "源码、固件、说明文档都在仓库里。", 31, MUTED, max_w=W - 2 * M)
    L.text(M, 446, "觉得好用，点个 ★ 让更多人发现它。", 31, MUTED, max_w=W - 2 * M)

    qr_path = Path(tempfile.gettempdir()) / "nba_board_repo_qr.png"
    segno.make(REPO, error="m").save(str(qr_path), scale=14, border=2,
                                    dark="#0A0E16", light="#FFFFFF")
    qr = Image.open(qr_path).convert("RGB").resize((430, 430), Image.LANCZOS)
    qx, qy = 92, 556
    L.d.rounded_rectangle((qx - 22, qy - 22, qx + 452, qy + 452), radius=28,
                          fill=(255, 255, 255, 245), outline=(255, 255, 255, 60), width=2)
    c.paste_rgb(qr, qx, qy)
    L = Layout(c.d)
    L.text(qx, qy + 474, "手机扫码 · 直接打开仓库", 26, MUTED,
           container=(M, qy + 452, 560, 1120), pad=4)

    rx = 626
    repo_box = (rx, 556, W - M, 766)
    L.d.rounded_rectangle(repo_box, radius=22,
                          fill=(255, 255, 255, 12), outline=(242, 160, 59, 90), width=2)
    L.text(rx + 28, 584, "GITHUB", 26, AMBER, bold=True, latin=True,
           container=repo_box, pad=20)
    L.text(rx + 28, 630, "LiLYzY54 /", 36, TEXT, bold=True, latin=True,
           max_w=W - M - (rx + 28) - 28, container=repo_box, pad=20)
    L.text(rx + 28, 684, "NBAWatchBoard", 36, TEXT, bold=True, latin=True,
           max_w=W - M - (rx + 28) - 28, container=repo_box, pad=20)

    items = ["全部场次的比分与节次", "分节比分 + 两队总分", "四组球员技术统计",
             "前后翻日 · 自适应刷新", "手机配网 · 多组 Wi-Fi"]
    iy = 808
    for i, item in enumerate(items, 1):
        L.text(rx, iy, "0" + str(i), 26, AMBER, bold=True, latin=True)
        L.text(rx + 58, iy, item, 29, TEXT, max_w=W - M - (rx + 58) - 8)
        iy += 74
    L.pill(rx, 1200, "欢迎 Star · Issue · PR", fg=AMBER,
           bg=(242, 160, 59, 20), size=24, outline=(242, 160, 59, 80), h=54,
           max_w=W - M - rx)

    cta = (M, 1358, W - M, 1486)
    L.d.rounded_rectangle(cta, radius=20,
                          fill=(255, 255, 255, 12), outline=(255, 255, 255, 34), width=2)
    L.text(104, 1386, REPO_TEXT, 30, TEXT, bold=True, latin=True,
           max_w=W - 2 * M - 64, container=cta, pad=16)
    L.text(104, 1442, "界面示意图 · 非实机照片", 22, DIM,
           container=cta, pad=16)
    return L


def main():
    specs = [
        ("cover.png", cover),
        ("gallery-1.png", lambda c: gallery(c, "01", "总览 · 一眼看全", "一个比赛日", "全在这一屏",
                                            "队名、比分、节次、开赛时间，上下翻就能看完。",
                                            SCREENS / "01-overview-screen.png",
                                            ["全天场次", "实时比分", "长按翻日"])),
        ("gallery-2.png", lambda c: gallery(c, "02", "详情 · 看懂过程", "点开一场", "看懂怎么赢",
                                            "Q1–Q4 分节比分、两队总分与战绩，一屏摆齐。",
                                            SCREENS / "02-detail-screen.png",
                                            ["分节比分", "两队总分", "比赛日"])),
        ("gallery-3.png", lambda c: gallery(c, "03", "球员 · 谁最热", "谁手感最热", "翻一页就知道",
                                            "得分、篮板、助攻……四组技术统计，按一下换一组。",
                                            SCREENS / "03-box-points-screen.png",
                                            ["四组数据", "一键切换", "名字常驻"])),
        ("gallery-4.png", qr_page),
    ]
    total, all_issues = 0, []
    for name, fn in specs:
        Layout.ALL = []
        c = Canvas()
        fn(c)
        out = OUT / name
        c.finish().save(out)
        im = Image.open(out)
        runs = sum(l.count for l in Layout.ALL)
        issues = [i for l in Layout.ALL for i in l.issues]
        total += runs
        all_issues += [f"[{name}] {i}" for i in issues]
        print(f"{name}: {im.size}, {out.stat().st_size / 1024:.0f} KiB, "
              f"{runs} text runs, {len(issues)} issues")
    print(f"--- total {total} text runs checked ---")
    if all_issues:
        print("LAYOUT FAILURES:")
        for i in all_issues:
            print("  " + i)
        raise SystemExit(1)
    print("LAYOUT OK: every text run fits its container")


if __name__ == "__main__":
    main()
