#!/usr/bin/env python3
"""零操作相册源:把照片丢进 src/ 即可,服务自动维护设备所需的一切。

设备「相册」App 需要:240×320 原图(≤18KB)、60×70 缩略图(~2KB,网格用)、
manifest.json(清单)。本服务在每次请求前按 (mtime,size) 增量同步 src/ →
web/:新照片自动转换,删照片自动清理,清单自动更新。全程无需手动跑脚本。

用法: python3 album_server.py [src_dir] [web_dir] [port]
默认: /data/src /data/web 8098(Docker 挂载约定)。
依赖: pillow(支持 jpg/png/webp/bmp/tiff;heic 需另装 pillow-heif)。
"""
import json
import os
import sys
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

SRC = sys.argv[1] if len(sys.argv) > 1 else "/data/src"
WEB = sys.argv[2] if len(sys.argv) > 2 else "/data/web"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8098
W, H = 240, 320
EXTS = {".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff"}
STATE_FILE = os.path.join(WEB, ".sync_state.json")

_sync_lock = threading.Lock()
_last_names = None  # 上次写出的清单,内容没变就不重写 manifest


def safe_name(stem, seen):
    """输出名清洗成纯 ASCII;重名加序号(与设备端 URL 转义约定配套)。"""
    safe = "".join(c if (c.isascii() and (c.isalnum() or c in "._-")) else "_" for c in stem)
    safe = safe.strip("_") or "photo"
    key = safe.lower()
    if key in seen:
        seen[key] += 1
        safe = f"{safe.rsplit('_', 1)[0]}_{seen[key]}" if "_" in safe else f"{safe}_{seen[key]}"
    else:
        seen[key] = 0
    return safe


def convert(path):
    """任意尺寸照片 → 240×320 封面裁切(短边贴满、居中)。"""
    img = Image.open(path).convert("RGB")
    ratio = max(W / img.width, H / img.height)
    resized = img.resize((max(1, round(img.width * ratio)), max(1, round(img.height * ratio))))
    left = (resized.width - W) // 2
    top = (resized.height - H) // 2
    cropped = resized.crop((left, top, left + W, min(resized.height, top + H)))
    if cropped.size != (W, H):
        canvas = Image.new("RGB", (W, H), (16, 18, 18))
        canvas.paste(cropped, (max(0, (W - cropped.width) // 2), 0))
        cropped = canvas
    return cropped


def sync():
    """增量同步 src/ → web/;新照片转原图+缩略图,删除的照片清掉产物。"""
    global _last_names
    os.makedirs(WEB, exist_ok=True)
    thumbs = os.path.join(WEB, "thumbs")
    state = {}
    if os.path.isfile(STATE_FILE):
        try:
            with open(STATE_FILE, encoding="utf-8") as fp:
                state = json.load(fp)
        except (OSError, ValueError):
            state = {}

    seen, entries, new_state = {}, [], {}
    for base in sorted(os.listdir(SRC)):
        path = os.path.join(SRC, base)
        stem, ext = os.path.splitext(base)
        if not os.path.isfile(path) or ext.lower() not in EXTS:
            continue
        try:
            st = os.stat(path)
        except OSError:
            continue
        name = safe_name(stem, seen) + ".jpg"
        sig = f"{st.st_mtime_ns}:{st.st_size}"
        if state.get(name) != sig:  # 新照片或内容变了
            os.makedirs(thumbs, exist_ok=True)
            try:
                cropped = convert(path)
            except Exception as exc:  # noqa: BLE001 - 坏图跳过继续
                print(f"skip {base}: {exc}", flush=True)
                continue
            target = os.path.join(WEB, name)
            for quality in range(80, 25, -5):  # 设备端缓冲档位 18KB
                cropped.save(target, "JPEG", quality=quality)
                if os.path.getsize(target) <= 18 * 1024:
                    break
            # 网格缩略图 60×70:对应原图中心 70/80 行(设备 1:1 解码直贴)
            cropped.resize((60, 80)).crop((0, 5, 60, 75)).save(
                os.path.join(thumbs, name), "JPEG", quality=75)
            print(f"ok  {base} -> {name}", flush=True)
        entries.append(name)
        new_state[name] = sig

    for name in state:  # src 里已删除的照片,清掉产物
        if name not in new_state:
            for stale in (os.path.join(WEB, name), os.path.join(thumbs, name)):
                try:
                    os.remove(stale)
                except OSError:
                    pass
            print(f"rm  {name}", flush=True)

    if entries and entries != _last_names:
        with open(os.path.join(WEB, "manifest.json"), "w", encoding="utf-8") as fp:
            json.dump({"rev": int(time.time()), "photos": entries}, fp, ensure_ascii=False)
        _last_names = entries
    with open(STATE_FILE, "w", encoding="utf-8") as fp:
        json.dump(new_state, fp)


class AlbumHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB, **kwargs)

    def do_GET(self):
        with _sync_lock:
            try:
                sync()  # 没变化时只是一圈 stat,毫秒级
            except Exception as exc:  # noqa: BLE001 - 同步失败不拦服务
                print(f"sync error: {exc}", flush=True)
        super().do_GET()


if __name__ == "__main__":
    with _sync_lock:
        sync()
    print(f"serving {WEB} on :{PORT} (auto-sync from {SRC})", flush=True)
    ThreadingHTTPServer(("0.0.0.0", PORT), AlbumHandler).serve_forever()
