# 官方上游 FoloToy/ai-passport 研究笔记（2026-11-03）

对官方 demo 仓库 <https://gitee.com/FoloToy/ai-passport>（GitHub 同名镜像）的完整调研：
主分支 + 全部 26 个远程分支逐一看过，并与本项目做了对照。结论先行：

- 官方 `main` **不是成品固件**，只是一块约 2000 行的"最小硬件测试基线"；
  本项目（launcher + 12 个应用）早已远超该基线。
- 官方真正有分量的是 11 个 `demo/*` / `feat/*` 示范分支，其中
  `claude-buddy-port`（约 7400 行）和 `senior-safety-card`（约 21600 行）
  对本项目后续功能有直接参考价值。
- BSP 同源但**引脚参数已分叉**，从官方分支抄代码时不能连带抄 BSP 数值（见 §4）。

## 1. 官方仓库定位

FoloToy AI Passport（AI 通行证）官方开源固件，MIT 协议。仓库是专门为
"AI 辅助开发"设计的：AGENTS.md 定义 AI 协作规范，`skills/` 内置五个
必需 AI 技能（setup / develop / build / device-test / debug），
`tools/validate.sh` 为交付门禁（static / firmware / full）。

组织思想：**`main` = 硬件能力契约，每个 `demo/*` 分支 = 从基线长出的
一个独立应用示范**。官方明确要求：二次开发必须从 `main` 拉新分支并
重新设计 UI，禁止照抄基线测试菜单；BSP API 与非 UI 逻辑可自由复用。
多个 demo 之间不要互相合并——它们教的是"模式"，不是功能堆叠。

## 2. 硬件平台契约（官方 main 已实现的接口)

| 能力 | 实现 | 应用接口 | 边界 |
| --- | --- | --- | --- |
| 显示 | ST7789P3 240×320 竖屏 RGB565 | `bsp_display_*` / `bsp_lvgl_*` | 无 PSRAM，单 DMA 小缓冲；无 MISO/触摸/TE |
| 输入 | UP/DOWN/OK 三键共用 GPIO0 ADC 分压 | `bsp_button_init()` | 回调在 button 任务里，不得阻塞 |
| 音频 | ES8311 I2S0 全双工 PCM | `bsp_audio_*` | PCM 读写阻塞，须放工作任务 |
| 电池 | CW2017 SOC/电压 | `bsp_battery_*` | 可缺省能力，读数≠标定 |
| Wi-Fi | 仅按需 STA 扫描 demo | `demo_wifi.c` | 不连接、不存凭证 |
| BLE | NimBLE 不可连接广播 `FoloPassport` | `demo_ble.c` | 无经典蓝牙 |
| 低功耗 | 浅睡 2s / 深睡 5s，RTC 唤醒 | `demo_low_power.c` | 强制暂停 ES8311 并回读校验 |
| 共享总线 | ES8311 与 CW2017 共用 I2C0 | `bsp_i2c_*` | 不得再建同端口总线 |

所有引脚常量只在 `components/bsp/include/bsp_pins.h` 定义（单一事实来源）。

## 3. 分支清单与价值

### demo/*（从 main 演化的应用示范）

| 分支 | 应用 | 可复用模式 |
| --- | --- | --- |
| `demo/claude-buddy-port` | 桌面 AI 硬件伴侣（+7400 行） | 加密 BLE 配对、协议解析、状态归约、任务通信、主机测试；完整应用替换 demo 菜单的标准范式 |
| `demo/tetris-game` | 三键俄罗斯方块 | 实时游戏循环、低延迟 PRESS 输入、局部刷新、纯游戏模型 + 音效 + 麦克风交互 |
| `demo/tamagezi` | 电子宠物（+2000 行） | 养成模型/存档(tamagezi_store)/音频分层 |
| `demo/funbox-11-in-1` | 十合一游戏合集 | RTTTL 蜂鸣音乐播放器(rtttl_player)、多游戏共用音频层 |
| `demo/cat-themed-pomodoro-timer` | 猫咪养成番茄钟 | 单调时钟、暂停/恢复、NVS 持久化、完整 PRD |
| `demo/rock-paper-scissors` | 猜拳 | RGB565 图片资产 + 素材生成脚本 + Flash 权衡 |
| `demo/blufi-provisioning` | BluFi 蓝牙 Wi-Fi 配网 | 乐鑫 blufi + 安全校验(demo_blufi_security)，配网备选路径 |
| `demo/stopwatch` | 秒表 | 最小计时应用、逻辑与 LVGL 分离、主机测试 |
| `demo/coloros-muyu` | 电子木鱼 | RGB565 背景资产 + 专用中文字体(font_muyu_22) |
| `demo/white-rabbit-pet` | 白兔宠物 | 基线内改菜单的轻量宠物 |
| `demo/recovery-ble` | BLE 恢复模式 | 救砖通道 |

### feat/* / fix/*（平台能力与维护）

| 分支 | 内容 |
| --- | --- |
| `feat/senior-safety-card` | **全仓库最大分支（+21600 行）**：JPEG 解码显示(jpeg_store/jpeg_view)、中文字体(lv_font_cn_16)、网页配置 portal(safety_portal)。相册若要支持 JPEG 源直接参考 |
| `feat/wireless-low-power-demos` | 无线 + 低功耗联合演示 |
| `feat/cw2017-custom-profile` | 电量计自定义 profile |
| `fix/wanli-500mah-battery-profile-20261020` | 500mAh 电池 profile 修正 |
| `fix/es8311-low-power-suspend` / `fix/es8311-sleep-vendored-docs` | ES8311 睡眠挂起修复 |
| `fix/demo-lifecycle-bsp-rollback` | demo 退出时 BSP 状态回滚 |
| `fix/remove-recovery-partition` / `fix/remove-recovery-boot-hook` | 移除恢复分区 |
| `codex/fix-battery-capacity-500mah` | 同电池容量修正 |

### docs/reference/（社区经验档案）

六个作者目录，含 SoftAP 配网资源预算、网络音频流与内存、BLE 手柄键盘
等实战文章。配网类需求先翻这里的 `phoenixzhc` 三篇。

## 4. 与本项目的关系

同平台（FoloToy-Card）二次开发。项目早期即从官方 BSP 衍生，之后独立演进。

**BSP 引脚已分叉，抄官方分支代码时以下数值不能跟官方走**（对照
`components/bsp/include/bsp_pins.h`）：

| 参数 | 官方 main | 本项目 | 原因 |
| --- | --- | --- | --- |
| LCD 时钟 | 80 MHz | 40 MHz | 本板实测稳定值 |
| 背光 LEDC 频率 | 5 kHz | 20 kHz | 5 kHz 升压电路可闻嗡鸣（<100% 亮度时） |
| 按键 ADC 窗口 | {150,447}/{447,1900}，松开 ≥1900 | {150,450}/{450,1000}，松开 ≥2500 | 本板分压电阻不同，按实测写 |
| 深睡按键唤醒 | 写入 bsp_pins.h | 未保留 | 本项目未用该路径 |

其余渊源：muse 应用在用 `buddy_font_16`（源自 `demo/claude-buddy-port`）；
muse 头像帧来自 Meta Muse Gadget SDK（见 `main/apps/muse/JOLLYBOT-NOTICE.md`，
授权单独说明）。官方工程约定与本项目现有红线一致：LVGL 非线程安全须持
`bsp_lvgl_lock()`、按键回调不阻塞、中文 UI 必须核对字形覆盖。

## 5. 对本项目的可借鉴清单（按优先级)

1. **JPEG 显示管线** ← `feat/senior-safety-card`：相册当前只收 WebP/预缩图，
   若要直出 JPEG 源（如更多相册服务端），jpeg_probe/store/view 三层可直接参考。
2. **蓝牙 Wi-Fi 配网** ← `demo/blufi-provisioning`：`main/launcher/wifi_setup.cc`
   现为 SoftAP/手动路径，BluFi 可提供"手机一键配网"体验；社区文档
   `docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md`
   有配网内存预算经验。
3. **游戏类应用标准范式** ← `demo/tetris-game`：低延迟 PRESS 输入 + 局部刷新 +
   纯模型分离（host tests 可测）。做新游戏时按此结构。
4. **电量 profile** ← `fix/wanli-500mah-battery-profile-20261020`：
   本机为 500mAh 电芯，读数若偏差可对照该分支的 CW2017 profile 写法。
5. **RTTTL 蜂鸣播放器** ← `demo/funbox-11-in-1`：无需音频文件即可放旋律，
   适合提示音/小彩蛋。

## 6. 本地参考克隆

官方仓库已克隆到本项目旁边的只读参考目录（含全部 26 个远程分支的浅层副本）：

```text
/Volumes/mac-extend/VibeCoding/ai-passport      # 只读参考，勿在此开发
```

丢失或需更新时重新克隆：

```bash
git clone https://gitee.com/FoloToy/ai-passport.git /Volumes/mac-extend/VibeCoding/ai-passport
cd /Volumes/mac-extend/VibeCoding/ai-passport
git fetch --depth 1 origin '+refs/heads/*:refs/remotes/origin/*'   # 拉全部分支
git diff main origin/demo/tetris-game -- main components           # 看某分支差异
```
