#!/usr/bin/env python
"""エクスプローラー関連付け用の「種別アイコン」を生成する開発ツール（要件3.3）。

pika を既定アプリにしたファイルがエクスプローラー上で表示するアイコン（レジストリ
``DefaultIcon``）を、ファイル種別ごとに別デザインで用意する。VSCode 風の
「文書（ページ）＋折れ角＋種別ラベル帯」で、配色は pika テーマ（src/styles/tokens.css）に準拠。

成果物（``md.ico`` 等の多解像度 .ico）はこのディレクトリにコミットしてあるため、
通常のビルドに Python は不要。**アイコンを足す/変えるときだけ**本スクリプトを実行する。

種類を増やす手順:
  1. 下の ``TYPES`` に 1 エントリ足す（progid / 拡張子 / ラベル / 配色 / フォント）。
  2. `python generate.py` を実行して .ico を再生成する（要 Pillow）。
  3. Rust 側 `crates/pika-core/src/explorer/mod.rs` の `ASSOC_TYPES` に同じ
     （progid / extensions / icon_file）を足す（登録ロジックの単一源）。
  4. `src-tauri/tauri.bundle.conf.json` の `bundle.resources` に新しい .ico を足す
     （NSIS インストーラで exe の隣へ同梱するため）。
  詳細は同ディレクトリの README.md を参照。

実行:
  python generate.py            # .ico を生成（このフォルダへ出力）
  python generate.py --preview  # 確認用 preview.png も併せて出力
"""
from __future__ import annotations

import os
import sys

from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
FONTS = "C:/Windows/Fonts/"

# ── 共通の配色（tokens.css 由来）─────────────────────────────────────────────
PAGE_FILL = (255, 255, 255, 255)
PAGE_BORDER = (190, 194, 202, 255)   # --border-2 近傍
FOLD_FILL = (232, 235, 240, 255)
TEXT_LINE = (200, 204, 212, 255)     # 本文を示す薄い罫線
ON_BAND = (255, 255, 255, 255)       # --on-accent（帯の上の白文字）

# ── 種別テーブル（ここを編集して種類を増やす）────────────────────────────────
#   name      : 出力ファイル名（<name>.ico）。Rust ASSOC_TYPES.icon_file と一致させる。
#   label     : 帯に載せる短いラベル（小サイズでも読める 2〜3 文字推奨）。
#   color     : 帯の塗り色（RGBA）。
#   font      : ラベルのフォント（C:/Windows/Fonts/ 配下）。
#   scale     : ラベルの大きさ（ページ幅に対する比）。文字数に応じて微調整する。
TYPES = [
    {
        "name": "md",
        "label": "M↓",
        "color": (61, 99, 160, 255),   # --accent-strong #3d63a0（Markdown=青）
        "font": "arialbd.ttf",
        "scale": 0.30,
    },
    {
        "name": "html",
        "label": "</>",
        "color": (180, 86, 38, 255),   # --alert 寄りの橙 #b45626（HTML=橙）
        "font": "consolab.ttf",
        "scale": 0.24,
    },
]

# .ico に詰める解像度（大→小）。
ICO_SIZES = [256, 128, 64, 48, 32, 24, 16]


def _render(size: int, color, label: str, font_path: str, scale: float) -> Image.Image:
    """1 枚のアイコンを高解像度で描いてから size へ縮小する（アンチエイリアス）。"""
    S = 1024
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)

    m = int(S * 0.16)
    left, top, right, bottom = m, int(S * 0.10), S - m, S - int(S * 0.10)
    fold = int(S * 0.18)
    r = int(S * 0.045)
    lw = max(2, S // 220)

    # 影
    shadow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle(
        [left + 8, top + 12, right + 8, bottom + 12], radius=r, fill=(0, 0, 0, 40)
    )
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(10)))

    # ページ本体（右上角を折るため角を欠いた多角形）
    page = [
        (left + r, top),
        (right - fold, top),
        (right, top + fold),
        (right, bottom - r),
        (right - r, bottom),
        (left + r, bottom),
        (left, bottom - r),
        (left, top + r),
    ]
    d.polygon(page, fill=PAGE_FILL)
    d.line(page + [page[0]], fill=PAGE_BORDER, width=lw, joint="curve")

    # 折れ角（めくれ）
    fold_poly = [(right - fold, top), (right, top + fold), (right - fold, top + fold)]
    d.polygon(fold_poly, fill=FOLD_FILL)
    d.line(fold_poly + [fold_poly[0]], fill=PAGE_BORDER, width=max(2, S // 260))

    # 本文を示す薄い罫線（3 本・最後は短く）
    lx0, lx1 = left + int(S * 0.10), right - int(S * 0.10)
    ly = top + fold + int(S * 0.06)
    gap = int(S * 0.075)
    for i in range(3):
        yy = ly + i * gap
        x1 = lx1 if i < 2 else lx0 + int((lx1 - lx0) * 0.6)
        d.rounded_rectangle(
            [lx0, yy, x1, yy + int(S * 0.022)], radius=int(S * 0.011), fill=TEXT_LINE
        )

    # 種別ラベル帯（下部・色塗り）。下端は角丸、上端は直線。
    band_top = bottom - int(S * 0.30)
    d.rounded_rectangle([left, band_top, right, bottom], radius=r, fill=color)
    d.rectangle([left, band_top, right, band_top + r], fill=color)

    # ラベル文字（中央寄せ）
    fs = int(S * scale)
    font = ImageFont.truetype(font_path, fs)
    tb = d.textbbox((0, 0), label, font=font)
    tw, th = tb[2] - tb[0], tb[3] - tb[1]
    tx = left + ((right - left) - tw) // 2 - tb[0]
    ty = band_top + ((bottom - band_top) - th) // 2 - tb[1]
    d.text((tx, ty), label, font=font, fill=ON_BAND)

    return img.resize((size, size), Image.LANCZOS)


def build(spec: dict) -> list[Image.Image]:
    font_path = FONTS + spec["font"]
    imgs = [_render(s, spec["color"], spec["label"], font_path, spec["scale"]) for s in ICO_SIZES]
    out = os.path.join(HERE, f"{spec['name']}.ico")
    imgs[0].save(out, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=imgs[1:])
    print("生成:", out)
    return imgs


def _checker(w: int, h: int, c: int = 16) -> Image.Image:
    bg = Image.new("RGBA", (w, h), (255, 255, 255, 255))
    dd = ImageDraw.Draw(bg)
    for y in range(0, h, c):
        for x in range(0, w, c):
            if (x // c + y // c) % 2 == 0:
                dd.rectangle([x, y, x + c, y + c], fill=(235, 235, 238, 255))
    return bg


def preview(out_path: str) -> None:
    sizes = [256, 64, 48, 32, 16]
    pad = 24
    row_w = sum(sizes) + pad * (len(sizes) + 1)
    row_h = 256 + pad * 2
    canvas = _checker(row_w, row_h * len(TYPES))
    for ri, spec in enumerate(TYPES):
        x, y0 = pad, ri * row_h
        for s in sizes:
            ic = _render(s, spec["color"], spec["label"], FONTS + spec["font"], spec["scale"])
            canvas.alpha_composite(ic, (x, y0 + (256 - s) // 2 + pad))
            x += s + pad
    canvas.convert("RGB").save(out_path)
    print("プレビュー:", out_path)


def main() -> int:
    for spec in TYPES:
        build(spec)
    if "--preview" in sys.argv:
        preview(os.path.join(HERE, "preview.png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
