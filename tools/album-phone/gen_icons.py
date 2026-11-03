#!/usr/bin/env python3
"""生成 App 图标:琥珀底 + 白色照片山景。跑一次即可,产物已入库无需重复跑。

用法: python3 gen_icons.py   (在 album-phone/ 目录下)
"""
from PIL import Image, ImageDraw
import os

AMBER = (221, 179, 108, 255)   # UI_ACCENT,与设备主题一致
WHITE = (255, 255, 255, 255)
BASE = 512


def draw_glyph(d, cx, cy, size):
    """在 (cx,cy) 中心画 size 大小的照片图形:圆角相框 + 太阳 + 双山。"""
    half = size / 2
    r = size * 0.16
    # 相框
    d.rounded_rectangle([cx - half, cy - half, cx + half, cy + half], radius=r, fill=WHITE)
    inner = size * 0.86
    ih = inner / 2
    d.rounded_rectangle([cx - ih, cy - ih, cx + ih, cy + ih], radius=r * 0.72, fill=AMBER)
    # 太阳
    sun_r = size * 0.10
    sx, sy = cx + inner * 0.20, cy - inner * 0.22
    d.ellipse([sx - sun_r, sy - sun_r, sx + sun_r, sy + sun_r], fill=WHITE)
    # 双山
    base_y = cy + ih * 0.55
    d.polygon([
        (cx - ih * 0.72, base_y),
        (cx - ih * 0.28, base_y - inner * 0.46),
        (cx + ih * 0.02, base_y - inner * 0.10),
        (cx + ih * 0.12, base_y - inner * 0.22),
        (cx + ih * 0.60, base_y),
    ], fill=WHITE)


def legacy(size, path):
    """方形圆角底 + 居中图形(老版本系统启动器直接用 PNG)。"""
    img = Image.new("RGBA", (BASE, BASE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    margin = BASE * 0.04
    d.rounded_rectangle([margin, margin, BASE - margin, BASE - margin],
                        radius=BASE * 0.20, fill=AMBER)
    draw_glyph(d, BASE / 2, BASE / 2, BASE * 0.62)
    img.resize((size, size), Image.LANCZOS).save(path)


def foreground(size, path):
    """自适应图标前景:透明底 + 缩到安全区的图形(背景层由 adaptive-icon 提供)。"""
    img = Image.new("RGBA", (BASE, BASE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    draw_glyph(d, BASE / 2, BASE / 2, BASE * 0.40)
    img.resize((size, size), Image.LANCZOS).save(path)


def main():
    res = os.path.join(os.path.dirname(__file__), "app", "src", "main", "res")
    legacy_sizes = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
    fg_sizes = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}
    for dpi, px in legacy_sizes.items():
        d = os.path.join(res, f"mipmap-{dpi}")
        os.makedirs(d, exist_ok=True)
        legacy(px, os.path.join(d, "ic_launcher.png"))
    for dpi, px in fg_sizes.items():
        d = os.path.join(res, f"mipmap-{dpi}")
        os.makedirs(d, exist_ok=True)
        foreground(px, os.path.join(d, "ic_launcher_foreground.png"))
    print("icons written")


if __name__ == "__main__":
    main()
