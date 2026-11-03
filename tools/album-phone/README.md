# 相册服务 App(Android 手机)

把 Mac 上的相册源(`tools/album/album_server.py`)搬进手机:装这个 App、
点「启动服务」、设备连手机热点,即可随时随地看相册。手机随手拍的新照片
会自动出现在设备相册里,删掉的照片自动消失——和 Mac 版一样零操作。

## 安装(一次性)

把 `album-server.apk` 传到手机(微信文件传输助手 / 网盘 / 数据线均可),
点开安装,允许"安装未知来源应用"。兼容 Android 7.0 及以上。

## 使用(每次出门 3 步)

1. **手机开热点**:把「AP 频段」选成 **2.4GHz**(设备只认 2.4GHz,不改必
   连不上),并关闭「无人连接时自动关闭热点」;
2. **App 里点「启动服务」**:首次会弹照片权限,选"允许访问所有照片";
   屏幕上会显示要填到设备里的地址;
3. **设备连热点 + 填地址**:设备的 WiFi 设置连手机热点;打开设备设置
   网页(平时往设备传照片用的那个),把相册源地址改成 App 绿框里显示的
   `http://192.168.x.x:8098`(每台手机固定,改一次以后不用再动)。

看完照片,通知栏点「停止」省电。在家想用 Mac 当相册源,把设备里的地址
改回 Mac 的 IP 即可,两边随时切换。

## 行为说明

- 照片范围默认"只收录相机拍摄的照片"(DCIM/Camera),界面可切换收录
  全部图片;
- 收录数量:全部收录、最新优先;设备清单池档位上限约 600 张(2026-11-03
  固件升级),超出自动截断并在状态行提示;每张原图压到 ≤7.5KB——设备
  堆被切碎时只剩 8K 拉取档,这个大小保证任何时刻都能看大图;
- **MIUI/HyperOS 必做**:系统设置 → 应用管理 → 相册服务 → 打开
  「自启动」+ 省电策略选「无限制」。否则熄屏后系统会把 App 冻结
  (端口还在但无人应答),设备端表现为"服务器连接失败",而 Mac 浏览器
  在屏幕亮时又能打开——极具迷惑性;
- 地址列表只显示真实 WiFi/热点网卡(已过滤 VPN/蜂窝干扰地址);
- 看相册的流量是设备↔手机点对点传输,**不耗流量套餐**(整本刷一遍约
  3MB);电台、语音助手走热点互联网,与相册互不影响;
- 服务运行期间持有唤醒锁(防熄屏休眠),主要耗电是热点本身;
- 照片整理(首次或大批新增)期间服务照常供应旧清单,不会挡住设备。

## 技术备注(给后续维护)

- 工程在 `tools/album-phone/`,Kotlin + 零第三方依赖(不用 AndroidX),
  `minSdk 24 / targetSdk 34`;UI 全代码生成,无 XML 布局。
- 服务行为与 Mac 版脚本对应:按 (mtime,size) 增量同步、240×320 中心
  裁切原图(80→25 递减 + 超限降分辨率,压到 ≤7.5KB)、60×70 缩略图、
  manifest.json(最新优先,预算 14.5K 对齐设备 16K 档)、文件名清洗成
  纯 ASCII(重名加 _N)、每次 HTTP 请求前增量对账(限频 2s,整理中
  短路放行旧清单)、删照片清产物。旧固件(清单 4KB)需升级后才能吃下
  全量清单。
- 照片源 = 系统媒体库 MediaStore(jpeg/png/webp/bmp/heic);相机模式
  API 29+ 按 `relative_path` 过滤,旧系统按 `bucket_display_name`。
- 构建(本机已配好,直接可用):
  ```bash
  export JAVA_HOME=$(/usr/libexec/java_home -v 17)
  export ANDROID_HOME=~/Library/Android/sdk
  ~/.gradle/wrapper/dists/gradle-9.1.0-all/2x09zxy9y9fz2e9j6blrf3xag/gradle-9.1.0/bin/gradle :app:assembleDebug
  # 产物 app/build/outputs/apk/debug/app-debug.apk(debug 签名,个人使用无碍)
  ```
- 模拟器验证过的点:一键启动、权限弹窗、manifest/原图/缩略图规格、
  中文名转 `photo.jpg`、png→jpg、新增/删除照片自动同步、路径越狱 404、
  截图默认过滤、状态页。
- 测试坑(模拟器特有):`adb push` 进 DCIM 的文件可能卡 `is_pending=1`
  (普通应用不可见),需
  `adb shell am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://<路径>`
  转正;真机相机拍的照片无此问题。另外宿主机 8098 被 Docker 相册源占用,
  `adb forward` 要换宿主端口(如 `tcp:18098 tcp:8098`)。
