# 相册源部署指南

设备端「相册」App 为纯在线模式:进应用时拉 `manifest.json` 清单,网格
拉缩略图(60×70),全屏看图拉原图(240×320 ≤18KB)。服务端只需 HTTP
静态提供这三样。

## 方式 A:Mac/任意有 Docker 的机器(推荐,零操作)

实际部署(Mac, 2026-11):栈在 `/Volumes/mac-extend/VibeCoding/album/`,
四个文件:`Dockerfile`(python:3.12-slim + pillow,清华 pip 源)、
`docker-compose.yml`、`album_server.py`(零操作自动同步服务)。

```bash
cd /Volumes/mac-extend/VibeCoding/album
docker compose up -d --build
# 之后:照片(任意尺寸 jpg/png/webp/bmp/tiff)丢进 src/ 即可,
# 删除同理。服务在每次请求前增量同步(mtime/size 对账,没变化时毫秒级):
# 自动生成 240×320 原图 + thumbs/ 60×70 缩略图 + manifest.json,
# 设备上配置 http://<这台机器的局域网IP>:8098
```

- 改了 `album_server.py` 必须 `docker compose build` 再 `up -d`
  (脚本是 COPY 进镜像的)。
- 同步状态记在 `web/.sync_state.json`;删掉它下个请求会全量重建。
- 相册源机器必须在设备所在局域网可达(设备没有 Tailscale/VPN)。
- heic 需在镜像里另装 pillow-heif,默认不支持。

## 方式 B:N150 / 任意 Linux(手动生成,无 Docker)

没有 Docker 时用 `make_album.sh`(ImageMagick)手动生成同样的产物,
配合任意静态文件服务:

```bash
sudo apt install -y imagemagick
mkdir -p ~/album/src ~/album/web
./make_album.sh ~/album/src ~/album/web     # 照片更新后重跑
sudo cp album-server.service /etc/systemd/system/   # python3 http.server :8098
sudo systemctl daemon-reload && sudo systemctl enable --now album-server
```

设备固件对两种方式完全兼容:有 `thumbs/` 走快速网格,没有则自动回退
拉原图(慢但可用)。

## 方式 C:Android 手机(热点随身,2026-11 新增)

相册服务已有 Android App 版(`../album-phone/`):手机开热点(2.4GHz)+
App 一键启动,设备连手机热点即可看相册,手机新拍的照片自动进设备相册。
安装与使用见 `../album-phone/README.md`。固件对三种方式完全兼容,区别
只是设备里配置的相册源地址。

## 容量与顺序

- manifest 顺序 = 文件名排序;想自定义顺序就把文件名前缀改成
  `001_/002_…`。手机相册源(tools/album-phone)按"最新优先"排序。
- 设备端单张上限:原图 18KB(脚本已按此压质量)、名字转纯 ASCII;
  手机源压 ≤7.5KB(设备堆碎时只剩 8K 拉取档,见 tools/album-phone)。
- 相册总量:清单池档位堆分配 16K/12K/8K/4K(2026-11-03 起),富余时
  约 600 张,堆紧张自动降档、池满按前缀截断;旧固件(4KB/192 张)对
  超大清单会整单失败,需升级固件配合手机源使用。
