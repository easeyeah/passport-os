# 手机相册源 + 相册容量升级排障实录(2026-11-03)

目标:把 Mac 上的相册源搬进用户的 Android 手机(Redmi/HyperOS),设备连
手机热点或同一 WiFi 即可看相册。当天从零做到全链路可用,连排五层问题。

## 第一层:手机 App(tools/album-phone/,新建)

Kotlin 零第三方依赖(不用 AndroidX)的相册服务 App:MediaStore 查询
(默认只收 DCIM/Camera)→ BitmapFactory 转换(240×320 原图 + 60×70
缩略图)→ 原生 socket HTTP(:8098)→ 前台服务 + 唤醒锁保活。固件零改动,
协议与 album_server.py 完全一致。构建/烧录/工具坑:
- 本机已有 Android Studio SDK + Gradle 9.1 缓存;AGP 8.13.1 + KGP 2.2.20
  与 Gradle 9.1 兼容;插件需 plugins{} 带版本写法(buildscript classpath
  在 Gradle 9 不再喂给 plugins 块)。
- **宿主机 8098 被 Docker 相册源占用**,adb forward 必须换宿主端口。
- **模拟器 adb push 进 DCIM 的文件会卡 `is_pending=1`**(普通 app 查询
  不可见,shell content query 看得到),需发 MEDIA_SCANNER_SCAN_FILE
  广播转正;真机相机照片无此问题。
- MIUI 禁 adb input tap(需"USB 调试(安全设置)"),远程点按钮行不通,
  装完 APK 后的"启动服务"一键只能用户手点。
- bounds 解析陷阱:块注释里写 `wlan*/ap*` 会提前终止注释(-Werror 同类坑)。

## 第二层:MIUI 冻结后台服务("Mac 能访问、设备连不上"的真凶)

症状诡异:Mac 浏览器有时能打开手机服务、有时连上 TCP 却 0 字节响应
(内核代答握手、无人 accept),USB adb 转发同样卡死。AMS dumpsys 显示
isFrozen=false、/proc/stat 全 'S'——**HyperOS 的后台省电在 CPU 调度层
把非自启动 App 饿到近乎零调度**,前台一亮瞬间恢复(对照实验 0.1s vs 超时)。
修复:系统设置里给 App 开「自启动」+ 省电策略「无限制」;App 侧 Doze
白名单(db deviceidle whitelist)+前台服务不够。教训:**"Mac 能访问"的
窗口期样本会误导诊断,必须区分"进程活"与"线程被调度"**。

## 第三层:设备清单 4KB 缓冲溢出("地址对了还是连不上")

手机 407 张的 manifest.json 有 10.6KB,设备清单池固定 4KB → 拉取溢出
→ 整单失败 → "服务器连接失败"。老 Mac 相册 81 张(2KB)从未触雷。
**先验证清单大小再怀疑网络**。修法(固件+手机端成对升级,一步到位):

### 固件(album_sync.cc 清单池档位化)
- kNamePoolBytes 4096 → **kPoolCaps {16K,12K,8K,4K} 档位堆分配**,名字
  均长 ~25B:16K≈600 名、12K≈450、8K≈300、最差 4K=旧行为。堆富余多收,
  紧张降档,分配失败路径与 fetch_to_heap 同构。
- **池满截断不再整单失败**:http_get_mem 加 truncated 出参,流式解析本
  就耐受截断(不完整名自然停),收录完整前缀,detail 提示"收录前N张"。
- **提交会话时把池收紧到实际用量**(malloc(used+1) + 原样拷贝,偏移相对
  基址不变),16K 富余块还回堆——长期工作集只有真实大小。
- 偏移表从栈(192×2B)挪到堆(768×2B),任务栈压力反降。
- 坑:malloc 指针不能交给 unique_ptr(char[] 用 delete[],-Werror=
  mismatched-new-delete),用本地 free-RAII 结构。
- BSS 零增长,SRAM 门禁 PASS(75496/73728)。

### 手机端
- 清单预算 3700B → 14500B(落 16K 档);照片压质从 ≤18KB 收紧到
  **≤7.5KB**——设备拉照片按 {18K,12K,8K} 档分配堆,堆碎时只剩 8K 档
  ("照片超过缓冲档位"日志),13KB 中位数的手机照片张张会撞,7.5KB
  (含余量)任何堆状态都能看;超限时逐级降分辨率(0.875×)再压。
- **syncIfStale 加 syncing 短路**:首轮全量整理(407 张 ≈50s)期间
  请求原会被 syncLock 排队到 8s 超时(设备"运行7秒后连接失败"的元凶),
  现在整理中直接放行、先供应旧清单。

## 第四层:相册固件两个存量 bug(407 张放大了它们)

- **s_photo_index 是 uint8_t**:256 张以上看图翻页索引溢出。连同
  album_show_photo 参数一并拓宽为 int,s_photo_index 改 atomic<int>
  (photo_next_index 在 input 线程读、album_task 写,原为数据竞争)。
- **看图返回不跟页**:back_to_grid_state 只重渲染进图前的旧页。改为
  返回时把 s_grid_page/s_grid_sel 同步到 s_photo_index 所在页(锁内
  原子更新,与复合状态纪律一致)——看图翻到第 20 页,返回就站在第 20 页。
- 串口驱动 UI 的经验:**截图流 ~15s 会撞熄屏,熄屏后第一键只唤醒**,
  键序列整体错位;以固件日志(render page N sel=M)为同步点比截图可靠。

## 第五层:小尺寸照片全屏残影("图片重叠")

原图压 ≤7.5KB 后又有 140 张高细节照片压不进,走降分辨率(210×280/
184×245/161×214)。设备全屏看图是"解码块 1:1 直推自己那块区域",
**全程无全屏清屏**——老 Mac 相册清一色 240×320 恰好盖满,从未暴露;
小图盖不住的部分保留上一张画面,用户所见即"图片重叠"。
修复(手机端,固件零改动):降档后贴 240×320 黑底画布(底色 16,18,18
与 Mac 版一致)居中,sig v4 强制重编码;407 张全部回归标准尺寸。
教训:**为压体积改输出尺寸时,必须确认下游渲染路径对非标尺寸的假设**;
抽验尺寸要过滤 wc/合计行的解析污染(两次踩到)。

## 验证

- build.sh "Project build complete";verify_firmware.py:布局 PASS、
  SRAM 75496/73728 PASS、合并镜像 PASS;烧录成功(共 3 版:档位池版、
  返回跟页版、精确分配版)。
- 真机:相册状态屏"407张 · 在线 堆77KB";网格翻页 34 页;看大图正常
  (手机端日志实锤原图拉取);返回落页日志实锤(render page 3 sel=7
  跟随所看照片);小尺寸残影消除(140 张全部回归 240×320)。
- 手机端:manifest 10661B/407 条;407 张全部 ≤6656B。
- 堆诊断取证:'v' 命令 free_heap/largest 双指标——free 够而 largest
  不够 = 碎片化,memory 任务还会打印最近分配失败的字节数序列。
- 代码推送:github.com/easeyeah/passport-os 的 album-phone 分支
  (2488611 主提交 → 0ba3207 精确分配 → 27f2935 黑底画布)。

## 待办

- 用户侧:省电策略「无限制」尚未确认设置(自启动已开);熄屏长时间后
  服务是否持续响应未做 5 分钟专项回归(被更高优先级工作打断)。
- 若未来照片数 >600:清单池首档 16K 是新上限,届时再做真分页协议
  (滑动窗口),需按第 22 轮纪律重审相册状态机。
