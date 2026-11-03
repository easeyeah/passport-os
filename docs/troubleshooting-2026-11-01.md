# 相册应用开发排障实录（2026-11-01）

从零开发「相册」App + 局域网门户改造 + Mac Docker 相册源的完整过程中遇到的
全部问题、根因与修复。按"环境 → 固件 → 应用逻辑"分层记录,每条含现象、
根因、修复,便于日后维护与避免重蹈。

## 一、构建环境(macOS + 外置盘)

### 1.1 GitHub 大文件下载卡死
- **现象**:idf_tools 下载 riscv 工具链(288MB)时 15 秒 0 字节假死,apt/curl 直连同理。
- **根因**:本网络 GitHub 直连不通,大文件传输会挂起而非快速失败。
- **修复**:一切 GitHub 大文件走 `https://gh-proxy.com/<原URL>` + `curl -C -` 断点续传 +
  `--speed-limit 10240 --speed-time 30` 限速检测自动重试。sha256 逐一校验。
- **涉及**:riscv32 工具链、cmake、ninja、actionlint 四个包全中过招。

### 1.2 macOS 上 idf_tools 不安装 cmake/ninja
- **现象**:export.sh 后 idf.py 报 `"cmake" must be available on the PATH`。
- **根因**:ESP-IDF 在 macOS 假设 cmake/ninja 由系统(Homebrew)提供,不在
  idf_tools 安装清单里;主硬盘紧张不能装 Homebrew。
- **修复**:手动把 dist/ 里已下载的 cmake/ninja 解压到外置盘,
  `ln -s` 进 `espressif/extra-bin/`,构建前 `PATH=.../extra-bin:$PATH`。

### 1.3 pip 阶段走清华镜像
- `export PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple` 防 pypi 直连卡死。

### 1.4 actionlint 安装脚本同样卡 GitHub
- validate.sh 首次运行会现场下载 actionlint → 同样卡死。预下载解压到
  `/tmp/actionlint-1.7.12/` 即可(脚本有缓存检查)。

## 二、固件编译与内存纪律

### 2.1 SRAM 预算是编译期硬门禁
- **现象**:verify_firmware.py 报 `first heap region 51656 < 73728`,构建失败。
- **根因**:相册初版静态 BSS 超标 +25.7KB(8KB 清单缓冲 + 17.5KB 全名缓存表)。
  这台设备无 PSRAM,固件对首堆区域有 73728 字节的硬预算。
- **修复**:缓存表条目从 64 字节原始名改为 12 字节 FNV-1a 哈希(160 项 → 2.6KB),
  8KB 清单缓冲从静态改任务内堆分配,清单上限 8192→4096。
- **教训**:**新增任何静态缓冲前先算 BSS 账;超了编译期直接被拒,不是运行时警告。**

### 2.2 LVGL 任务栈溢出(相册进入即闪退)
- **现象**:进相册 300ms 内整机重启。
- **根因**:UI 刷新跑在 LVGL 任务(栈仅 7168B),每 tick 把 2.6KB 的 album_cache_t
  整表拷贝到栈上,叠加 LVGL 自身渲染占用即溢出。
- **修复**:store 层提供零拷贝访问器(`album_store_count()`/`album_store_hash_at()`),
  LVGL 上下文禁止任何大结构体栈拷贝。
- **教训**:**小栈任务(LVGL 7168 / httpd 6144)里禁用按值返回大结构的接口。**

## 三、主机测试的 macOS 兼容(上游按 Linux GCC 编写)

按撞见顺序,全部已修并回传上游可用的写法:

| 问题 | 根因 | 修复 |
|---|---|---|
| app_base.h 编译拒 | 匿名 struct + C++ 默认成员初始化器触发 clang `-Wnon-c-typedef-for-linkage` | `typedef struct passport_app {...}` 加 tag 名 |
| test-host.sh 链接失败 | Apple ld 不认 `-Wl,--gc-sections` | Darwin 分支改 `-Wl,-dead_strip` |
| phone-time 测试 | `.c` 文件混进 clang++ 命令触发 `-Wdeprecated` | cJSON 用 cc 单独编译成 .o 再链接 |
| test_radio_catalog | `kTag` 在主机 mock 下未使用触发 `-Wunused-const-variable` | 加 `[[maybe_unused]]` |
| test_xiaozhi / test_muse_readiness | 同 cJSON 混编问题 | 拆分编译 |
| muse 三兄弟测试 | ① `-Werror` 下三方 cJSON 的 sprintf 被 macOS SDK 标记 deprecated ② `/opt/esp-idf` 硬编码未读 IDF_PATH ③ 生成代码里 mock 函数未使用 | 加 `-Wno-deprecated-declarations` / 读环境变量 / mock 加 `__attribute__((unused))` |
| test_muse_tls_memory | Apple ld 无 `--wrap`;且宏级联替换(`__real_X→esp_X→__wrap_X`)会自递归栈溢出 | 编译期宏三名字互不重叠:真身改名 `real_*`,调用点改名 `__wrap_*`,`__real_*` 直指 `real_*`;链接命令必须带 `-fsanitize` |
| radio_catalog.cc | `kTag` 主机构建下未使用 | `[[maybe_unused]]` |

**通用教训:主机测试编译命令里,`.c` 文件永远不要直接进 `c++` 命令行;
GNU ld 专属选项(--wrap/--gc-sections)必须做平台分支。**

## 四、应用逻辑 bug(自研代码)

### 4.1 /api/album 保存全部被拒
- **现象**:门户保存报"设备未能保存,请重试"。
- **根因**:`cJSON_IsString(obj)` 误判了根对象(应为判断 `url` 字段),
  所有合法请求都被 400。
- **教训**:cJSON 校验永远针对**取出的字段**,不是根节点。

### 4.2 相册同步"有进度但 0 张"(三层叠加,逐层剥开)
最终根因链(由表及里):
1. **URL 拼接缺分隔斜杠(真正元凶)**:照片名无前导 `/`,base+name 拼成
   `:8098yuzijiang_3.jpg`,端口吞噬文件名。清单路径自带 `/` 所以一直正常,
   极具迷惑性。修复:build_url 自动补 `/`。
2. 文件名含空格/非 ASCII 时 URL 解析同样被拒 → build_url 增加 RFC3986
   百分号转义;make_album.py 输出名清洗为纯 ASCII。
3. 同步任务内存超标(8KB 清单缓冲 + cJSON 解析树 ~4KB + 8KB 栈)把
   ~30KB 基线堆耗尽至 0KB,连锁出"空名字条目""client创建失败"等假象。
   → **甩掉 cJSON,清单改流式字节扫描器**(自家格式逐名处理),
   缓冲 4KB、任务栈 6144,同步期间堆占用 ~20KB → ~2KB。
- **教训**:esp_http_client_init 返回 NULL 十有八九是 URL 解析拒绝,
  先打印完整 URL 再怀疑内存;屏显诊断行(见 4.3)是定位手段。

### 4.3 设备控制台不走 USB 口
- **现象**:pyserial 打开 /dev/cu.usbmodem1101 永远 0 字节(该口只能烧录)。
- **修复**:放弃串口,把诊断写进**相册状态屏**:`diag()` → `s_detail[160]` →
  `album_sync_detail()` → 状态屏实时显示迭代数/失败名+长度/末错/堆曲线。
  "URL 路径尾部"这一针最终钉死元凶。
- **教训**:本固件的调设备日志只有屏显一条路;诊断信息会被后续覆盖,
  排障时把关键信息并进最终汇总行。

### 4.4 流式扫描器两处连带 bug(重构引入,一处被我修一处)
- 删除回扫起点用了旧偏移量(`arr_pos + strlen("photos":[")`),arr_pos 语义
  变化后会扫漏开头条目导致误删缓存 → 起点改为 arr_pos 本身。
- 扫描器最初要求 `"photos":[` 无空格,而 json.dump 默认输出 `"photos": [` →
  报"清单格式错误"。修复:键名定位容忍冒号前后空白。
- **教训**:批量字符串替换改代码后,必须全文重读改动区域;
  派生指针的语义变化要全文排查引用点。

## 五、门户与页面

### 5.1 输入框样式不统一
- 全局输入框 CSS 选择器只列了 `type=text/password`,新增的 `type=url` 未覆盖。
- 修复:选择器补 `input[type=url]`。**新增控件类型时检查全局样式选择器覆盖。**

### 5.2 页面 JS 语法错误
- python 批量替换时丢分号导致 SyntaxError,test_profile_page.js 当场拦截。
- **教训:改内嵌 JS 必须重跑 node 测试;批量替换的字符串要带上下文边界。**

## 六、产品层面

- 局域网门户(方案一):设备已联网时「设置→手机设置」不再切热点,直接局域网
  开设置页;修复了 Wi-Fi 省电档导致的 3KB/s(APP_ID_WIFI 提到 BALANCED)。
- 门户服务跟随界面开关(进"手机设置"才监听,退出即关)是有意的省内存设计,
  不是 bug——文档与屏显文案已说明。
- 「你在狗叫什么」下线:registry/CMake/测试桩全部摘除,源码保留,
  枚举位保留防 ID 位移。
- Tailscale 地址(100.100.7.64)对设备不可达:设备无 Tailscale 客户端,
  相册源必须在设备所在局域网(Mac Docker 方案落地)。
