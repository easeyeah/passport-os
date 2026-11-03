#!/usr/bin/env bash
# 把普通照片目录变成 AI Passport 相册源:
#   照片(任意尺寸) -> 240x320 封面裁切 JPEG(≤48KB) + manifest.json
# 用法: ./make_album.sh <照片目录> <输出目录>
# 依赖: ImageMagick(magick 或 convert)。手机照片怎么进 <照片目录> 由你定
# (iCloud/百度盘同步盘、Syncthing、手动拷贝均可),脚本只负责转换。
set -euo pipefail

SRC="${1:?用法: make_album.sh <照片目录> <输出目录>}"
OUT="${2:?用法: make_album.sh <照片目录> <输出目录>}"

IM=magick
command -v magick >/dev/null 2>&1 || IM=convert
command -v "$IM" >/dev/null 2>&1 || { echo "需要 ImageMagick: apt install imagemagick" >&2; exit 1; }

mkdir -p "$OUT" "$OUT/thumbs"
shopt -s nullglob nocaseglob

photos=()
for f in "$SRC"/*.jpg "$SRC"/*.jpeg "$SRC"/*.png "$SRC"/*.webp "$SRC"/*.heic; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    name="${base%.*}.jpg"
    target="$OUT/$name"
    # 240x320 cover 裁切;质量从 80 起步,超 48KB 就降,直到 40 或达标
    q=80
    while :; do
        "$IM" "$f" -auto-orient -resize 240x320^ -gravity center -extent 240x320 \
            -quality "$q" "$target" 2>/dev/null || { echo "跳过(转换失败): $base" >&2; target=""; break; }
        size=$(wc -c < "$target" | tr -d ' ')
        [ "$size" -le 49152 ] || [ "$q" -le 40 ] || { q=$((q-10)); continue; }
        break
    done
    [ -n "$target" ] || continue
    # 设备网格缩略图 60×70(等比缩 60×80 后裁中间 70 行)
    "$IM" "$target" -resize 60x80! -crop 60x70+0+5 +repage -quality 75 \
        "$OUT/thumbs/$name" 2>/dev/null || true
    photos+=("$name")
    echo "ok  $base -> $name (${size}B, q$q)"
done

if [ "${#photos[@]}" -eq 0 ]; then
    echo "照片目录里没有可识别的图片(jpg/png/webp/heic)" >&2
    exit 1
fi

# 按文件名排序保证相册顺序稳定;rev 用时间戳,设备靠它判断要不要增量拉取
mapfile -t photos < <(printf '%s\n' "${photos[@]}" | sort)
{
    printf '{"rev":%s,"photos":[' "$(date +%s)"
    for i in "${!photos[@]}"; do
        [ "$i" -gt 0 ] && printf ','
        printf '"%s"' "${photos[$i]}"
    done
    printf ']}\n'
} > "$OUT/manifest.json"

echo "完成: ${#photos[@]} 张 -> $OUT (manifest rev $(date +%s))"
