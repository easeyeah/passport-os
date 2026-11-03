// 相册 App(纯在线模式):状态屏(LVGL)/网格总览/全屏看图 三态。
// 网格:逐格拉服务端缩略图(thumbs/,60×70,约 2KB)1:1 解码贴进 3×4 格,
// 旧相册源无缩略图时回退拉原图 1/4 抽点;页眉有加载进度条与页码点阵;
// 选中框画在格子间预留的 3px 黑边上,可无损擦除。
// 全屏:原图 1:1 分块解码直推面板。两者都绕过 LVGL(失效暂停),零落盘。
#include "apps/album/album_sync.h"
#include "launcher/app_registry.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "app_fonts.h"
#include "ui_pixel.h"
#include "wifi_manager.h"
#include "esp_http_client.h"
#include "esp_system.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace {
constexpr const char *kTag = "album";
constexpr int kMcuHeight = 16;
constexpr std::size_t kJpegWorkSize = 4096;
constexpr int64_t kSlideshowIntervalUs = 5000000;
constexpr int kPanelW = 240;
constexpr int kPanelH = 320;

// 网格几何:页眉 16px + 4 行×76px;格 80×76 = 3px 边距 + 74×70 内区。
// 缩略 = 原图 1/4 解码(60×80)裁掉上下各 5 行,水平居中;格子间 3px 黑边
// 是选中框的绘制区(纯黑,可无损擦除重画)。
constexpr int kGridHeaderH = 16;
constexpr int kCellW = 80, kCellH = 76;
constexpr int kCellMargin = 3;
constexpr int kCellInnerW = kCellW - kCellMargin * 2; // 74
constexpr int kCellInnerH = kCellH - kCellMargin * 2; // 70
constexpr int kGridCols = 3, kGridRows = 4;
constexpr int kGridPerPage = kGridCols * kGridRows;
constexpr int kThumbRadius = 4; // 缩略图圆角,与速算24 卡片(4px)风格一致
constexpr int kPhotoRadius = 6; // 全屏照片圆角,与速算24 面板(6px)风格一致

std::atomic<bool> s_active{false};
lv_obj_t *s_screen;
lv_obj_t *s_status_label;
lv_obj_t *s_hint_label;
lv_timer_t *s_timer;
ui_pixel_battery_t s_battery{};
lv_obj_t *s_wifi_bars[3] = {nullptr, nullptr, nullptr};

// 界面模式。原子量:input 任务(app 任务)与 LVGL 任务都要读。
enum class UiMode : int { STATUS, GRID, PHOTO };
std::atomic<int> s_mode{static_cast<int>(UiMode::STATUS)};
std::atomic<int> s_photo_index{-1}; // 当前全屏张:int——uint8 在 256+ 张时溢出
bool s_photo_from_grid; // 全屏是否从网格进入(OK 回网格而非状态屏)
bool s_slideshow;
int64_t s_last_advance_us;
int s_grid_page;
int s_grid_sel;
int s_grid_render_page = -1; // 正在渲染的页(-1 = 需要渲染)
album_sync_status_t s_prev_status = ALBUM_SYNC_IDLE; // ui_tick 自动进网格的沿检测
std::mutex s_photo_mutex;    // 模式/页码状态在 input、album 任务间互斥
// 面板单写者纪律:input 任务只置状态,所有直推绘制都在 album_task。
// 此前两任务并发画面板 + 按键音效并发分配,快速连按容易把小堆挤爆 panic。
// 帧状态三件套(prev/sel/dirty)必须配对读写:s_photo_mutex 保护,
// 否则并发下会按错误格子擦框,留下永远擦不掉的"孤儿高亮框"。
bool s_frame_dirty = false;             // 同页挪框请求(album_task 消费)
int s_frame_prev_sel = 0;               // 待擦除的旧框格(与 dirty 配对)
std::atomic<int> s_photo_req_index{-1}; // 全屏看图/翻页请求索引(album_task 消费)
bool s_photo_req_from_grid = false;     // 随请求一起写
// 最近一次跨页导航时刻:渲染防抖——按住键连发跨页时等停键再渲染,
// 否则每次键击都打断重启渲染,屏幕反复清黑永远填不满。
std::atomic<int64_t> s_nav_last_us{0};

std::atomic<bool> s_panel_busy{false}; // 直推写者在画(render/看图):true。
// 面板交还 LVGL(ui_show_status)前必须等它变 false——否则 LVGL 的渲染
// 与在途直推并发,flush 完成信号被代答,LVGL 抱锁睡死(整机冻结)。

// LVGL 失效开关的夹逼读写(2026-11-03,排障第二十四轮真凶):LVGL 9 的
// enable_invalidation 是计数制(true/false 各 ±1),而相册的禁用点比恢复
// 点多——每走一遍"进网格→看图→返回"计数净 -1,几轮后漂成负数,恢复点
// 的单次 +1 永远凑不回正数,状态屏从此不再重绘(面板残留旧帧,即"长按
// 返回没反应")。夹逼后计数恒 ∈ {0,1}:禁用只在开着时关,恢复循环开到正。
// 两个辅助都要求调用方已持 bsp_lvgl_lock。
void lvgl_invalidation_off_guarded() {
    lv_display_t *disp = lv_display_get_default();
    if (disp && lv_display_is_invalidation_enabled(disp))
        lv_display_enable_invalidation(disp, false);
}

void lvgl_invalidation_on_reset() {
    lv_display_t *disp = lv_display_get_default();
    if (!disp) return;
    for (int guard = 0; guard < 10 && !lv_display_is_invalidation_enabled(disp); ++guard)
        lv_display_enable_invalidation(disp, true);
}

// LVGL 心跳 + 冻结自愈:ui_tick(跑在 LVGL 任务)每 300ms 心跳 +1;
// 相册自愈任务发现心跳停 10s(屏幕亮着)即重启设备——TCB 被腐蚀的
// 冻结(2026-11-03 取证实锤)无法在线恢复,自动重启取代"永远卡死"。
std::atomic<uint32_t> s_lvgl_hb{0};

// 直推写者的 RAII 标记:作用域内 panel busy。
struct PanelBusyGuard {
    explicit PanelBusyGuard() { s_panel_busy = true; }
    ~PanelBusyGuard() { s_panel_busy = false; }
};

// 面板访问总闸:相册的每一段面板写入都必须持 bsp_lvgl_lock(与屏幕系统
// 同一把锁)。esp_lcd 的"设窗口→写数据"序列不是多任务安全的——相册任务
// 与 LVGL 刷新任务的写序列交错时,数据落错窗口、flush 完成信号错乱,
// 表现为花屏直至屏幕任务永久卡死。锁内禁止再取 s_photo_mutex(锁序:
// 全系统统一 photo_mutex → lvgl,反向即死锁)。
struct LvglGate {
    LvglGate() { bsp_lvgl_lock(-1); }
    ~LvglGate() { bsp_lvgl_unlock(); }
};

TaskHandle_t s_task_handle;

// —— JPEG 流式解码公共件 ——

struct PhotoJpegContext {
    esp_http_client_handle_t client = nullptr;
    uint16_t *stripe = nullptr;
    uint16_t width = 240;
    uint16_t height = 320;
    bool color_pending = false;
    bool failed = false;
    // 网格模式:缩略图内区屏幕原点
    int grid_x = 0, grid_y = 0;
    bool grid_mode = false;
    int sample_step = 4; // 抽样步长:服务端缩略图=1(1:1),原图=4(1/4 抽点)
    // 内存源(在线模式:原图先拉进 RAM 再解码,设备不落盘)
    const uint8_t *mem_data = nullptr;
    size_t mem_len = 0, mem_pos = 0;
};

std::size_t photo_jpeg_input(JDEC *decoder, uint8_t *buffer, std::size_t requested) {
    auto *context = static_cast<PhotoJpegContext *>(decoder->device);
    if (!context) return 0;
    if (context->mem_data) { // 内存源:确定性读写
        if (!buffer) {
            const size_t skip = std::min(requested, context->mem_len - context->mem_pos);
            context->mem_pos += skip;
            return skip;
        }
        const size_t avail = context->mem_len - context->mem_pos;
        const size_t n = std::min(requested, avail);
        memcpy(buffer, context->mem_data + context->mem_pos, n);
        context->mem_pos += n;
        return n;
    }
    if (!context->client) return 0;
    if (buffer) {
        const int n = esp_http_client_read(context->client,
            reinterpret_cast<char *>(buffer), static_cast<int>(requested));
        return n <= 0 ? 0 : static_cast<std::size_t>(n);
    }
    // 跳过 N 字节:HTTP 流只能读掉丢弃
    char scratch[64];
    size_t left = requested;
    while (left) {
        const int n = esp_http_client_read(context->client, scratch,
            static_cast<int>(left < sizeof(scratch) ? left : sizeof(scratch)));
        if (n <= 0) break;
        left -= static_cast<size_t>(n);
    }
    return requested - left;
}

uint16_t rgb565_panel(uint32_t rgb) {
    return static_cast<uint16_t>(
        ((((rgb >> 16) & 0xF8U)) << 8) |
        ((((rgb >> 8) & 0xFCU)) << 3) |
        ((rgb & 0xFFU) >> 3));
}

void apply_frame_move(); // 前置:render_grid_page 在定义前使用

// 圆角遮罩:返回 true 表示像素落在四角圆弧之外(网格里不画、露黑底;
// 全屏里写黑)。中部列或中部行永不裁;图太小(任一边 < 2r)不裁。
bool pixel_rounded_off(int x, int y, int w, int h, int r) {
    if (r <= 0 || w < 2 * r || h < 2 * r) return false;
    if ((x >= r && x < w - r) || (y >= r && y < h - r)) return false;
    const int dx = x < r ? r - x : x - (w - 1 - r);
    const int dy = y < r ? r - y : y - (h - 1 - r);
    return dx * dx + dy * dy > r * r;
}

// 全屏 1:1 输出:整行直推。
int photo_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<PhotoJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;
    if (s_mode.load() != static_cast<int>(UiMode::PHOTO)) {
        context->failed = true; // 已退出看图:中止解码,别再往面板贴
        return 0;
    }
    const uint16_t block_width = rectangle->right - rectangle->left + 1;
    const uint16_t block_height = rectangle->bottom - rectangle->top + 1;
    if (rectangle->right >= context->width || rectangle->bottom >= context->height) {
        context->failed = true;
        return 0;
    }
    if (rectangle->left == 0 && context->color_pending && bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
    context->color_pending = false;
    uint16_t *target = context->stripe;
    if (!target) { context->failed = true; return 0; }

    // TJPGD 输出 BGR888,转面板 RGB565(字节序交换后直推)。
    // 四角圆弧外的像素写黑(全屏后面板底色未知,必须显式覆盖)。
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    for (uint16_t y = 0; y < block_height; ++y) {
        uint16_t *row = target + static_cast<std::size_t>(y) * context->width;
        for (uint16_t x = 0; x < block_width; ++x) {
            const std::size_t input = (static_cast<std::size_t>(y) * block_width + x) * 3U;
            const uint16_t color = pixel_rounded_off(
                rectangle->left + x, rectangle->top + y,
                context->width, context->height, kPhotoRadius)
                ? 0
                : static_cast<uint16_t>(
                      ((bgr[input + 2] & 0xf8U) << 8U) |
                      ((bgr[input + 1] & 0xfcU) << 3U) |
                      (bgr[input] >> 3U));
            row[rectangle->left + x] = __builtin_bswap16(color);
        }
    }
    if (rectangle->right + 1U == context->width) {
        if (esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, rectangle->top,
                                      context->width, rectangle->bottom + 1, target) != ESP_OK) {
            context->failed = true;
            return 0;
        }
        context->color_pending = true;
    }
    return 1;
}

// 网格输出:按 sample_step 抽点贴格(服务端缩略图 step=1 即 1:1;原图
// step=4 即 1/4 抽点,不用 TJpgDec 缩放模式以兼容任意 TJPGD 构建)。
// TJpgD 按 16px MCU 块回调,块内抽样列必须映射到 (ax-x0)/step 的缩略图
// 列号再贴,不能每块都从格子左缘排起——否则 15 个块相互覆盖,每格只剩
// 4px 竖缝。目标缩略 60×70:列窗口取 src_w 居中 60*step,行窗口同理。
int grid_jpeg_output(JDEC *decoder, void *bitmap, JRECT *rectangle) {
    auto *context = static_cast<PhotoJpegContext *>(decoder->device);
    if (!context || !bitmap || !rectangle || context->failed) return 0;
    if (s_mode.load() != static_cast<int>(UiMode::GRID)) {
        context->failed = true; // 已退出网格:中止解码,别再往面板贴
        return 0;
    }
    const auto *bgr = static_cast<const uint8_t *>(bitmap);
    const int step = context->sample_step;
    const int src_w = std::min<int>(decoder->width, kPanelW);
    const int src_h = std::min<int>(decoder->height, kPanelH);
    const int x0 = std::max(0, (src_w - 60 * step) / 2);
    const int y0 = std::max(0, (src_h - kCellInnerH * step) / 2);
    const int sw = rectangle->right - rectangle->left + 1;
    const int ax0 = rectangle->left, ay0 = rectangle->top;
    for (int ry = 0; ry < rectangle->bottom - rectangle->top + 1; ++ry) {
        const int ay = ay0 + ry;      // 原图行
        if (ay < y0 || ay >= y0 + kCellInnerH * step || (ay - y0) % step != 0) continue; // 抽样行
        const int dy = context->grid_y + (ay - y0) / step;
        uint16_t row[60];
        int first = -1, used = 0;
        for (int rx = 0; rx < sw; ++rx) {
            const int ax = ax0 + rx;
            if (ax < x0 || ax >= x0 + 60 * step || (ax - x0) % step != 0) continue; // 抽样列
            const int tx = (ax - x0) / step;
            if (pixel_rounded_off(tx, (ay - y0) / step, 60, kCellInnerH, kThumbRadius)) continue;
            const std::size_t input = (static_cast<std::size_t>(ry) * sw + rx) * 3U;
            const uint16_t color = static_cast<uint16_t>(
                ((bgr[input + 2] & 0xf8U) << 8U) |
                ((bgr[input + 1] & 0xfcU) << 3U) |
                (bgr[input] >> 3U));
            if (first < 0) first = tx;
            row[used++] = __builtin_bswap16(color);
        }
        if (used &&
            esp_lcd_panel_draw_bitmap(bsp_display_panel(), context->grid_x + first, dy,
                                      context->grid_x + first + used, dy + 1, row) != ESP_OK) {
            context->failed = true;
            return 0;
        }
    }
    return 1;
}

// —— 网格辅助 ——

// 网格选中框:画(或清除)格子 3px 边距上的琥珀色边框。边距纯黑,可无损重画。
uint16_t s_frame_h[kCellW * kCellMargin];
uint16_t s_frame_v[kCellMargin * kCellInnerH];

// 页眉进度条:一格一段(16×4px 细条,顶行;下行左侧电池、右侧页码)。
// 渐进填充时一眼可辨:段在走=网络慢,段不动=拉图卡死,设备重启=崩溃。
uint16_t s_prog_seg[16 * 4];

// 页眉页码:5×7 经典点阵(1px 宽笔画 × 2px 高,细体不占格),右侧对齐。
// 直推模式下没有字体渲染,手工点阵(数字 + '/')。
const uint8_t kDigit5x7[11][7] = {
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}, // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}, // 9
    {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}, // /
};

void grid_draw_page_indicator(int page, int pages) {
    LvglGate gate; // 面板写总闸
    char text[24];
    std::snprintf(text, sizeof(text), "%d/%d", page + 1, std::max(1, pages));
    const int n = static_cast<int>(std::strlen(text));
    constexpr int kGlyphH = 14, kAdvance = 7; // 5×7,笔画 1 宽 × 2 高
    const int w = n * kAdvance - 2;
    if (n <= 0 || w > 60) return;
    uint16_t buf[kGlyphH * 60];
    const uint16_t on = __builtin_bswap16(rgb565_panel(UI_ACCENT));
    for (int i = 0; i < kGlyphH * w; ++i) buf[i] = 0;
    for (int i = 0; i < n; ++i) {
        const char c = text[i];
        const int glyph = (c >= '0' && c <= '9') ? c - '0' : 10;
        for (int py = 0; py < 7; ++py)
            for (int px = 0; px < 5; ++px)
                if (kDigit5x7[glyph][py] & (0b10000 >> px))
                    for (int sy = 0; sy < 2; ++sy)
                        buf[(py * 2 + sy) * w + i * kAdvance + px] = on;
    }
    esp_lcd_panel_draw_bitmap(bsp_display_panel(), kPanelW - w - 4, 1,
                              kPanelW - 4, 1 + kGlyphH, buf);
    // 屏障:本函数的栈缓冲下一次调用会被复用(render 循环里紧跟着
    // grid_clear_cell 的栈缓冲),排空在途传输防其读到被复写的栈内存。
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
}

// 拉取失败的格子标记:左上角一小段强调色(24×3px)。曾用满格色块,
// 与选中框混淆且被用户当成"残留高亮";小标记一眼可辨是错误指示。
void grid_mark_failed(int cell_x, int cell_y) {
    LvglGate gate; // 面板写总闸
    uint16_t mark[24 * 3];
    const uint16_t v = __builtin_bswap16(rgb565_panel(UI_ACCENT));
    for (auto &px : mark) px = v;
    esp_lcd_panel_draw_bitmap(bsp_display_panel(), cell_x + kCellMargin + 2,
                              cell_y + kCellMargin + 2,
                              cell_x + kCellMargin + 26, cell_y + kCellMargin + 5,
                              mark);
    // 屏障:mark 是栈缓冲,下一格的 grid_clear_cell/grid_mark_failed 会
    // 复用同一栈地址(round-14 教训:栈缓冲 + 异步 DMA 必须排空再复用)。
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
}

void grid_draw_frame(int cell, bool accent) {
    LvglGate gate; // 面板写总闸(屏障的排空也在闸内,天然独占)
    const int col = cell % kGridCols, row = cell / kGridCols;
    const int x0 = col * kCellW, y0 = kGridHeaderH + row * kCellH;
    const uint16_t v = rgb565_panel(accent ? UI_ACCENT : 0x000000);
    for (auto &px : s_frame_h) px = __builtin_bswap16(v);
    for (auto &px : s_frame_v) px = __builtin_bswap16(v);
    auto *panel = bsp_display_panel();
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x0 + kCellW, y0 + kCellMargin, s_frame_h);                       // 上
    esp_lcd_panel_draw_bitmap(panel, x0, y0 + kCellH - kCellMargin, x0 + kCellW,
                              y0 + kCellH, s_frame_h);                                                        // 下
    esp_lcd_panel_draw_bitmap(panel, x0, y0 + kCellMargin, x0 + kCellMargin,
                              y0 + kCellH - kCellMargin, s_frame_v);                                          // 左
    esp_lcd_panel_draw_bitmap(panel, x0 + kCellW - kCellMargin, y0 + kCellMargin,
                              x0 + kCellW, y0 + kCellH - kCellMargin, s_frame_v);                             // 右
    // 屏障:draw_bitmap 是异步排队传输(tx_color → spi_device_queue_trans),
    // 返回时 DMA 可能还没读 s_frame_h/v。调用方"擦旧框→画新框"会立刻把
    // 这对静态缓冲重填成新颜色——不排空队列,在途的"擦除"传输读到的就是
    // 重填后的高亮色,把擦除画成了孤儿高亮框。发一个 NOP 命令强制排空
    // (tx_param 走 polling 且先等在途传输完成,与 bsp_display_fill 同款)。
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
}

// 清空一格(黑底,含边距),随后解码的缩略图贴在内区。
void grid_clear_cell(int cell) {
    LvglGate gate; // 面板写总闸
    const int col = cell % kGridCols, row = cell / kGridCols;
    const int x0 = col * kCellW, y0 = kGridHeaderH + row * kCellH;
    uint16_t black[kCellW * kCellMargin];
    for (auto &px : black) px = 0;
    for (int sy = 0; sy < kCellH; sy += kCellMargin) {
        const int h = std::min(kCellMargin, kCellH - sy);
        esp_lcd_panel_draw_bitmap(bsp_display_panel(), x0, y0 + sy, x0 + kCellW,
                                  y0 + sy + h, black);
    }
    // 屏障:black 是栈缓冲,下一格调用时同地址被再次清零前还会经历深栈
    // 的解码调用链(jd_decomp 等),不排空在途 DMA 会把垃圾读进已提交的
    // 清黑传输(round-14 教训)。
    if (bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
}

// 拉取并解码第 index 张贴进 (cell_x, cell_y) 格子内区。优先服务端缩略图
// (60×70,约 2KB,1:1 解码,快);旧相册源没有 thumbs/ 时回退拉原图 1/4
// 抽点(慢但兼容)。work 由整页渲染统一分配(一页一次,减少堆分配
// 次数——满格失败色块曾是"右下角残留高亮"的元凶)。失败画小标记。
bool grid_decode_cell(int index, int cell_x, int cell_y, uint8_t *work) {
    uint8_t *buf = nullptr;
    size_t len = 0;
    const bool thumb = album_sync_fetch_thumb(index, &buf, &len);
    if (!thumb && !album_sync_fetch_photo(index, &buf, &len)) {
        std::free(buf);
        if (s_mode.load() == static_cast<int>(UiMode::GRID)) grid_mark_failed(cell_x, cell_y);
        return false;
    }
    bool ok = false;
    if (work && s_mode.load() == static_cast<int>(UiMode::GRID)) {
        LvglGate gate; // 面板写总闸:解码+贴图期间独占,fetch 已在闸外完成
        PhotoJpegContext context = {
            .width = kPanelW,
            .height = kPanelH,
            .grid_x = cell_x + kCellMargin + 7, // 60 列缩略图在 74 宽内区居中
            .grid_y = cell_y + kCellMargin,     // 首行贴内区顶
            .grid_mode = true,
            .sample_step = thumb ? 1 : 4,
            .mem_data = buf,
            .mem_len = len,
        };
        JDEC decoder;
        JRESULT res = jd_prepare(&decoder, photo_jpeg_input, work, kJpegWorkSize, &context);
        if (res == JDR_OK) {
            res = jd_decomp(&decoder, grid_jpeg_output, 0); // 抽点/1:1 在回调里
        }
        ok = res == JDR_OK && !context.failed;
    }
    std::free(buf);
    if (!ok && s_mode.load() == static_cast<int>(UiMode::GRID)) grid_mark_failed(cell_x, cell_y);
    return ok;
}

// 渲染一页网格:整屏清黑(含页眉)→ 逐格拉原图 → 抽样解码贴格(渐进填充,
// 每格约 0.4s)。失败的格子画琥珀实块;翻页/退出即刻中止。
void render_grid_page(int page) {
    // 入口模式守卫:调用方的 mode 是循环顶快照,走到这里时可能已经切走
    // (ui_show_status 等)。整屏清黑先于逐格守卫,绝不能在非 GRID 态开画。
    if (s_mode.load() != static_cast<int>(UiMode::GRID) || !s_active.load()) return;
    PanelBusyGuard busy; // 整页直推期间标记 busy,ui_show_status 据此等待
    ESP_LOGI(kTag, "render page %d sel=%d start", page, s_grid_sel);
    s_grid_render_page = page;
    { // 整屏清黑:页眉区域与格子底色一次铺好,随后逐格渐进填充
        LvglGate gate; // 面板写总闸
        uint16_t black[kPanelW];
        for (auto &px : black) px = 0;
        for (int y = 0; y < kPanelH; ++y)
            esp_lcd_panel_draw_bitmap(bsp_display_panel(), 0, y, kPanelW, y + 1, black);
        // 屏障:320 笔在途传输全部读的是同一栈缓冲,块一结束 black 就出
        // 作用域、地址随即被页码点阵等后续栈帧复用,先排空(round-14)。
        if (bsp_display_io()) {
            esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
        }
    }
    const int count = album_sync_count();
    grid_draw_page_indicator(page, (count + kGridPerPage - 1) / kGridPerPage);
    // 解码工作缓冲整页只分配一次(此前每格一次,堆碎片时末尾格子
    // 分配失败被画成满格错误块——"右下角残留高亮"的真正来源)
    std::unique_ptr<uint8_t[], decltype(&std::free)> work(
        static_cast<uint8_t *>(std::malloc(kJpegWorkSize)), std::free);
    if (!work) {
        s_grid_render_page = -1; // 堆暂缺 4KB 连续块:黑屏,150ms 后重试
        return;
    }
    const int start_cell = s_grid_sel; // 渲染期间选中框还会动,顺序只在起点快照一次
    for (int k = 0; k < kGridPerPage; ++k) {
        const int cell = (start_cell + k) % kGridPerPage; // 选中格最先渲染,翻页体感更快
        if (s_mode.load() != static_cast<int>(UiMode::GRID) ||
            !s_active.load() ||
            s_grid_page != page) return; // 用户已翻页/退出应用,中止
        if (s_photo_req_index.load() >= 0) return; // 要看图:立刻让位
        esp_task_wdt_reset(); // 整页渲染 6-12s,超过 WDT 窗口,逐格喂
        apply_frame_move(); // 渲染间隙即时响应同页挪框
        const int index = page * kGridPerPage + cell;
        const int cx = (cell % kGridCols) * kCellW;
        const int cy = kGridHeaderH + (cell / kGridCols) * kCellH;
        grid_clear_cell(cell);
        if (index < count) grid_decode_cell(index, cx, cy, work.get());
        { // 页眉进度条点亮一段(细条,顶行)
            LvglGate gate; // 面板写总闸
            const uint16_t v = __builtin_bswap16(rgb565_panel(UI_ACCENT));
            for (auto &px : s_prog_seg) px = v;
            esp_lcd_panel_draw_bitmap(bsp_display_panel(), k * 16, 1,
                                      k * 16 + 16, 5, s_prog_seg);
            // 屏障:s_prog_seg 是静态缓冲,下一格循环立刻按同色重填——
            // 同色重填虽不可见,仍按纪律排空,防未来改色时踩 round-14。
            if (bsp_display_io()) {
                esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
            }
        }
    }
    {
        // 页码与选中格在锁内配对读取:两条独立读之间被按键更新,
        // 会把别页的格子画成本页的孤儿选中框(右缘高亮残留的来源)
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        const int sel = s_grid_sel;
        const bool same_page = (s_grid_page == page);
        if (same_page) grid_draw_frame(sel, true);
    }
    ESP_LOGI(kTag, "render page %d done", page);
}

// —— 直推模式进出 ——

void ui_show_status() {
    s_mode = static_cast<int>(UiMode::STATUS); // 先停:render/解码的模式守卫随即中止
    // 取消网格待渲染:album_task 主循环的 mode 是循环顶的旧快照,可能在
    // 本函数恢复 LVGL 失效**之后**才走进 render_grid_page——它的整屏清黑
    // (无条件、先于逐格守卫)会直接压掉刚恢复的状态屏,表现为"长按返回
    // 没反应"。置 render_page=page 让渲染待办检查落空,旧快照窗口随之关闭。
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        s_grid_render_page = s_grid_page;
    }
    // 面板交还 LVGL 前等直推写者收工(最长 2s;写者收尾后的残余绘制都有
    // 模式守卫,超时放行也不会再有面板写入)。album_task 自己调用时跳过
    // (等自己 = 死锁)。
    const bool external = xTaskGetCurrentTaskHandle() != s_task_handle;
    int waited_ms = 0;
    if (external) {
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 40 && s_panel_busy.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
        }
        ESP_LOGI(kTag, "ui_show_status: busy_wait=%dms panel_busy=%d", waited_ms,
                 static_cast<int>(s_panel_busy.load()));
    }
    bsp_lvgl_lock(-1);
    lvgl_invalidation_on_reset(); // 计数夹逼:把可能漂负的失效计数拉回开启
    if (s_screen) lv_obj_invalidate(s_screen);
    // 取证(2026-11-03):"长按返回没反应"的真凶是失效计数漂负——这行日志
    // 应恒为 inv=1;若再现 inv=0 说明计数平衡被新代码破坏,立即停手排查。
    lv_display_t *disp = lv_display_get_default();
    ESP_LOGI(kTag, "ui_show_status: inv=%d active=%p own=%p",
             disp ? lv_display_is_invalidation_enabled(disp) : -1,
             reinterpret_cast<void *>(lv_screen_active()), reinterpret_cast<void *>(s_screen));
    bsp_lvgl_unlock();
}

void enter_grid_mode(int page, int sel) {
    const int count = album_sync_count();
    if (count <= 0) return;
    album_sync_cancel_reset(); // 粘滞取消会让进网格后的所有拉图秒失败
    const int pages = (count + kGridPerPage - 1) / kGridPerPage;
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    if (sel < 0) sel = 0;
    const int cells = std::min(kGridPerPage, count - page * kGridPerPage);
    if (sel >= cells) sel = cells - 1;
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        bsp_lvgl_lock(-1);
        s_mode = static_cast<int>(UiMode::GRID);
        s_grid_page = page;
        s_grid_sel = sel;
        s_grid_render_page = -1;
        lvgl_invalidation_off_guarded();
        bsp_lvgl_unlock();
    }
}

// 状态回网格:面板已被全屏图覆盖,请求 album_task 重渲染。只改状态,
// input 任务可安全调用(面板单写者纪律)。
// 落点跟随"正在看的那张":看图期间的翻页只动 s_photo_index,网格的页/格
// 必须同步跟上,否则返回的是进图前的旧页(翻到 20 页看图,回来站在 1 页)。
void back_to_grid_state() {
    std::lock_guard<std::mutex> guard(s_photo_mutex);
    bsp_lvgl_lock(-1);
    s_mode = static_cast<int>(UiMode::GRID);
    const int idx = s_photo_index.load();
    const int count = album_sync_count();
    if (idx >= 0 && idx < count) {
        s_grid_page = idx / kGridPerPage;
        s_grid_sel = idx % kGridPerPage;
    }
    s_grid_render_page = -1;
    lvgl_invalidation_off_guarded();
    bsp_lvgl_unlock();
}

// 全屏看完/加载失败后返回:来源是网格就重渲染网格,否则回状态屏。
// 回网格必须把 mode 设回 GRID——面板已被全屏图覆盖,靠 album_task 重渲染。
void exit_photo_mode() {
    bool to_grid = false;
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        if (s_mode.load() != static_cast<int>(UiMode::PHOTO)) return;
        s_slideshow = false;
        to_grid = s_photo_from_grid;
    }
    if (to_grid) {
        back_to_grid_state();
        return;
    }
    ui_show_status();
}

// 纯在线:原图拉进内存(不落盘),再从内存分块解码直推面板。
bool album_show_photo(int index) {
    PanelBusyGuard busy; // 全屏直推期间标记 busy
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!album_sync_fetch_photo(index, &buf, &len)) {
        ESP_LOGW(kTag, "fetch photo %d failed: %s", index, album_sync_detail());
        return false;
    }
    if (s_mode.load() != static_cast<int>(UiMode::PHOTO)) {
        std::free(buf); // 拉取期间已退出看图,别再往面板贴图
        return false;
    }
    LvglGate gate; // 面板写总闸:解码+直推期间独占(fetch 已在闸外完成)
    auto *work = static_cast<uint8_t *>(std::malloc(kJpegWorkSize));
    auto *stripe = static_cast<uint16_t *>(
        std::malloc(kPanelW * kMcuHeight * sizeof(uint16_t)));
    if (!work || !stripe) {
        if (work) std::free(work);
        if (stripe) std::free(stripe);
        std::free(buf);
        ESP_LOGE(kTag, "decode alloc failed");
        return false;
    }
    PhotoJpegContext context = {
        .stripe = stripe,
        .width = kPanelW,
        .height = kPanelH,
        .mem_data = buf,
        .mem_len = len,
    };
    JDEC decoder;
    JRESULT res = jd_prepare(&decoder, photo_jpeg_input, work, kJpegWorkSize, &context);
    if (res == JDR_OK) {
        if (decoder.width > kPanelW || decoder.height > kPanelH) {
            ESP_LOGW(kTag, "photo %ux%u exceeds panel", decoder.width, decoder.height);
            res = JDR_FMT1;
        } else {
            context.width = decoder.width;
            context.height = decoder.height;
            res = jd_decomp(&decoder, photo_jpeg_output, 0);
        }
    }
    if (context.color_pending && bsp_display_io()) {
        esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    }
    std::free(work);
    std::free(stripe);
    std::free(buf);
    if (res != JDR_OK || context.failed) {
        ESP_LOGW(kTag, "decode failed idx=%d res=%d", index, res);
        return false;
    }
    return true;
}

// 真正进入全屏看图(album_task 专属:状态切换+拉图解码画屏都在这里,
// 保证面板单写者)。失败:来源网格回网格重渲染,否则回状态屏。
void enter_photo_now(int index, bool from_grid) {
    album_sync_cancel_reset(); // 同上:防止粘滞取消秒拒看图拉取
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        bsp_lvgl_lock(-1);
        s_photo_index = index;
        s_photo_from_grid = from_grid;
        if (from_grid) s_slideshow = false; // 清掉幻灯片残留标志,防止误自动连播
        s_mode = static_cast<int>(UiMode::PHOTO);
        lvgl_invalidation_off_guarded();
        bsp_lvgl_unlock();
    }
    if (album_show_photo(index)) return;
    ESP_LOGW(kTag, "photo %d load failed", index);
    if (from_grid) {
        back_to_grid_state();
    } else {
        ui_show_status();
    }
}

// 已在全屏看图模式下切到指定张(album_task 专属)。
// 坏图自动跳下一张,避免卡死在单张损坏文件上。
void flip_to(int index) {
    std::lock_guard<std::mutex> guard(s_photo_mutex);
    if (s_mode.load() != static_cast<int>(UiMode::PHOTO)) return;
    s_photo_index = index;
    s_last_advance_us = esp_timer_get_time();
    if (album_show_photo(index)) return;
    const int count = album_sync_count();
    if (count > 1) {
        s_photo_index = (index + 1) % count;
        album_show_photo(s_photo_index);
    }
}

// 网格内移动选择。到头/到尾停住(不循环绕圈):末页选到最后一张再往下
// 没有下一页,首页选到第一张再往上没有上一页。同页只请求挪框(零网络
// 零解码,album_task ≤150ms 内画好);跨页请求整页重渲染(停键防抖后)。
// 本函数只改状态不画屏——面板单写者在 album_task。
void grid_move(int delta) {
    const int count = album_sync_count();
    if (count <= 0) return;
    int abs_idx = s_grid_page * kGridPerPage + s_grid_sel + delta;
    if (abs_idx < 0) abs_idx = 0;              // 首页头部:停住
    if (abs_idx >= count) abs_idx = count - 1; // 末页尾部:停住
    const int new_page = abs_idx / kGridPerPage;
    const int new_sel = abs_idx % kGridPerPage;
    if (new_page == s_grid_page && new_sel == s_grid_sel) return;
    if (new_page != s_grid_page) {
        {
            // 与渲染收尾的读配对加锁:复合状态(页+格)必须原子更新,
            // 否则收尾会把别页的选中格画到当前页,留下无人擦的孤儿框
            std::lock_guard<std::mutex> guard(s_photo_mutex);
            s_grid_page = new_page;
            s_grid_sel = new_sel;
            s_grid_render_page = -1;
        }
        s_nav_last_us = esp_timer_get_time();
        return;
    }
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        s_frame_prev_sel = s_grid_sel;
        s_grid_sel = new_sel;
        s_frame_dirty = true; // album_task 统一"擦旧画新"
    }
}

// 应用同页挪框(album_task 专属;渲染间隙与空闲轮询都会调)。
// prev/sel/dirty 在锁内配对取走,不会读到混合的新旧对。
void apply_frame_move() {
    int prev = 0, cur = 0;
    {
        std::lock_guard<std::mutex> guard(s_photo_mutex);
        if (!s_frame_dirty) return;
        s_frame_dirty = false;
        prev = s_frame_prev_sel;
        cur = s_grid_sel;
    }
    if (s_mode.load() != static_cast<int>(UiMode::GRID)) return;
    grid_draw_frame(prev, false);
    grid_draw_frame(cur, true);
}

// 已在全屏看图模式,由 input 任务发出的翻页方向(dir = ±1)。
int photo_next_index(int dir) {
    const int count = album_sync_count();
    if (count <= 0) return -1;
    return (s_photo_index.load() + count + dir) % count;
}

// —— 状态屏(LVGL) ——

void set_text(lv_obj_t *obj, const char *text) {
    if (std::strcmp(lv_label_get_text(obj), text) != 0) lv_label_set_text(obj, text);
}

void refresh_status_ui() {
    if (!s_screen) return;
    const album_sync_status_t status = album_sync_status();
    const int count = album_sync_count();
    char line[96];
    static int64_t running_since_us = -1; // RUNNING 段起点(卡死排障:显示秒数+堆)
    if (status == ALBUM_SYNC_RUNNING && s_prev_status != ALBUM_SYNC_RUNNING) {
        running_since_us = esp_timer_get_time();
    }
    switch (status) {
        case ALBUM_SYNC_RUNNING: {
            const int secs = running_since_us > 0
                ? static_cast<int>((esp_timer_get_time() - running_since_us) / 1000000) : 0;
            std::snprintf(line, sizeof(line), "正在获取照片列表 %ds\n堆%uKB",
                          secs, static_cast<unsigned>(esp_get_free_heap_size() / 1024));
            break;
        }
        case ALBUM_SYNC_NO_URL:
            std::snprintf(line, sizeof(line), "未配置相册服务器\n手机设置里填写相册地址");
            break;
        case ALBUM_SYNC_NO_WIFI:
            std::snprintf(line, sizeof(line), "设备未联网\n在线相册需要网络");
            break;
        case ALBUM_SYNC_NET_FAIL:
            std::snprintf(line, sizeof(line), "服务器连接失败\n检查地址后按确定重试");
            break;
        case ALBUM_SYNC_BAD_MANIFEST:
            std::snprintf(line, sizeof(line), "相册清单格式错误\n请检查服务器 manifest");
            break;
        case ALBUM_SYNC_OK:
            std::snprintf(line, sizeof(line), "%d张·在线 堆%uKB", count,
                          static_cast<unsigned>(esp_get_free_heap_size() / 1024));
            break;
        default:
            std::snprintf(line, sizeof(line), count ? "%d 张照片" : "相册是空的\n按确定获取照片列表", count);
            break;
    }
    set_text(s_status_label, line);
    if (s_mode.load() != static_cast<int>(UiMode::STATUS)) return;
    const char *detail = album_sync_detail();
    if (status == ALBUM_SYNC_OK && detail[0] != '\0' &&
        std::strncmp(detail, "完成", 6) != 0) {
        // 网格里最近一次照片拉取的失败原因(如"照片 HTTP 404")。
        // 本设备无串口日志,状态屏是唯一诊断输出口。
        set_text(s_hint_label, detail);
    } else if (status == ALBUM_SYNC_NO_URL) {
        set_text(s_hint_label, "手机设置里可配置");
    } else {
        set_text(s_hint_label, "确定 网格浏览 · 长按上 幻灯片");
    }
}

void ui_tick(lv_timer_t *timer) {
    const bool visible = app_registry_screen_is_on();
    lv_timer_set_period(timer, visible ? 300 : 1000);
    s_lvgl_hb.fetch_add(1, std::memory_order_relaxed); // 心跳:亮屏/熄屏都要跳(LVGL 活着的证据)
    if (!visible) return;
    if (s_mode.load() != static_cast<int>(UiMode::STATUS)) return;
    bool auto_enter = false;
    if (bsp_lvgl_lock(100)) {
        refresh_status_ui();
        static uint32_t battery_tick;
        const album_sync_status_t st = album_sync_status();
        if (st == ALBUM_SYNC_OK && s_prev_status == ALBUM_SYNC_RUNNING) auto_enter = true;
        s_prev_status = st;
        if (s_screen && lv_tick_get() - battery_tick >= 5000) {
            ui_pixel_battery_set(&s_battery, bsp_battery_soc());
            // 金丝雀(2026-11-03):STATUS 态下 LVGL 失效必须常开;若被关,
            // 说明有路径关了失效却没恢复(状态屏永远不再刷新,"按键没反应"
            // 的头号嫌疑),每 5s 巡检一次直接上日志。
            lv_display_t *canary = lv_display_get_default();
            if (canary && !lv_display_is_invalidation_enabled(canary)) {
                ESP_LOGW(kTag, "canary: invalidation DISABLED while STATUS mode!");
            }
            const bool wifi_ok = WifiManager::GetInstance().IsConnected();
            int bars = 0;
            uint32_t wifi_col = UI_TEXT_DIM;
            if (wifi_ok) {
                const int rssi = WifiManager::GetInstance().GetRssi();
                bars = rssi >= -60 ? 3 : (rssi >= -75 ? 2 : 1);
                wifi_col = UI_TEXT;
            }
            ui_pixel_wifi_bars_set(s_wifi_bars, bars, wifi_col);
            battery_tick = lv_tick_get();
        }
        bsp_lvgl_unlock();
    } else {
        ESP_LOGW(kTag, "ui_tick: lvgl lock busy >100ms"); // 冻结取证:锁被谁长持
    }
    if (auto_enter && s_mode.load() == static_cast<int>(UiMode::STATUS) &&
        album_sync_count() > 0) {
        enter_grid_mode(0, 0);
    }
}

// 常驻小任务:面板唯一写者。消费 input 任务的状态请求(挪框/看图/
// 翻页),渲染网格页,推进幻灯片,保活。
void album_task(void *) {
    // 挂 WDT:卡死(如阻塞在网络调用)时 5s 打印调用栈,取证用。
    // 阻塞型 fetch 最长 8s > 5s 会触发两次转储,属已知噪声。
    if (TaskHandle_t self = xTaskGetCurrentTaskHandle()) esp_task_wdt_add(self);
    while (s_active.load()) {
        const int mode = s_mode.load();
        if (mode == static_cast<int>(UiMode::GRID) ||
            mode == static_cast<int>(UiMode::PHOTO)) {
            app_registry_keep_awake();
        }
        const int req = s_photo_req_index.exchange(-1);
        if (req >= 0) { // 看图/翻页请求优先(渲染循环里也会让位)
            const int m = s_mode.load();
            if (m == static_cast<int>(UiMode::GRID)) {
                enter_photo_now(req, s_photo_req_from_grid);
            } else if (m == static_cast<int>(UiMode::PHOTO)) {
                flip_to(req);
            }
        } else if (mode == static_cast<int>(UiMode::GRID)) {
            apply_frame_move(); // 同页挪框(空闲时)
            if (album_sync_status() == ALBUM_SYNC_OK &&
                s_grid_render_page != s_grid_page &&
                esp_timer_get_time() - s_nav_last_us.load() >= 200000) { // 停键防抖
                render_grid_page(s_grid_page);
            }
        } else if (mode == static_cast<int>(UiMode::PHOTO) && s_slideshow &&
                   esp_timer_get_time() - s_last_advance_us >= kSlideshowIntervalUs) {
            const int next = photo_next_index(1);
            if (next >= 0) s_photo_req_index = next; // 走统一请求路径
            else s_slideshow = false;
        }
        for (int i = 0; i < 3 && s_active.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50)); // 短轮询:请求尽快被消费
            esp_task_wdt_reset();
        }
    }
    s_task_handle = nullptr;
    esp_task_wdt_delete(xTaskGetCurrentTaskHandle()); // 摘狗再自删,防句柄悬空
    vTaskDelete(nullptr);
}

// 是否有未完成的加载/渲染(清单拉取、网格页渲染待办、在途直推)。
// 供系统在"长按确定退出"时判断:忙则推迟退出,加载完成自动续办。
extern "C" bool album_ui_busy(void) {
    if (album_sync_status() == ALBUM_SYNC_RUNNING) return true; // 清单拉取中
    if (s_active.load() && s_mode.load() == static_cast<int>(UiMode::GRID) &&
        s_grid_render_page != s_grid_page) return true; // 网格页渲染待办/进行中
    if (s_panel_busy.load()) return true; // 在途直推
    return false;
}

// 冻结自愈:LVGL 任务的心跳(ui_tick 每 300ms)停 10s 且屏幕亮着,即
// 判定界面冻结(TCB 腐蚀类,无法在线恢复),重启设备自愈。全程约 10s。
void album_watch_task(void *) {
    uint32_t last = s_lvgl_hb.load();
    int stall_s = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_active.load()) { stall_s = 0; continue; } // 只在相册前台时监护
        // 熄屏不豁免:LVGL 冻结后 sys_mon 60s 会熄屏,看熄屏状态会漏判
        const uint32_t now = s_lvgl_hb.load();
        if (now != last) { last = now; stall_s = 0; continue; }
        if (++stall_s >= 10) {
            ESP_LOGE(kTag, "LVGL frozen %ds — self-recovery reboot", stall_s);
            vTaskDelay(pdMS_TO_TICKS(200)); // 让这条日志发出去
            esp_restart();
        }
    }
}

// —— 生命周期 ——

void init() {}

void start() {
    ESP_LOGI(kTag, "Starting Album app");
    album_sync_init();

    bsp_lvgl_lock(-1);
    if (s_screen) { bsp_lvgl_unlock(); return; }
    s_screen = lv_obj_create(nullptr);
    ui_pixel_background(s_screen);
    lv_obj_t *top = ui_pixel_top_bar(s_screen);
    ui_pixel_wifi_bars_create(top, 12, 18, s_wifi_bars);
    lv_obj_t *name = ui_pixel_label(top, "相册", &buddy_font_16, UI_TEXT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 42, 0);
    s_battery = ui_pixel_battery_create(top, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());

    s_status_label = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT);
    lv_obj_set_pos(s_status_label, 12, 96);
    lv_obj_set_width(s_status_label, 216);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    s_hint_label = ui_pixel_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM);
    lv_obj_set_pos(s_hint_label, 12, 168);
    lv_obj_set_width(s_hint_label, 216);
    lv_label_set_long_mode(s_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    ui_pixel_footer(s_screen, "", "长按确定返回", &buddy_font_16);
    refresh_status_ui();
    lv_screen_load(s_screen);
    bsp_lvgl_unlock();

    s_active = true;
    s_timer = lv_timer_create(ui_tick, 300, nullptr);
    if (xTaskCreate(album_task, "album_task", 6144, nullptr, 4, &s_task_handle) != pdPASS) {
        s_task_handle = nullptr;
        ESP_LOGE(kTag, "album task create failed");
    }
    if (xTaskCreate(album_watch_task, "album_watch", 3072, nullptr, 1, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "album watch task create failed");
    }
    // 进应用自动刷新一次清单
    album_sync_refresh_start();
}

bool prepare_stop() {
    // 用户要求:列表加载/渲染未完成时不退出——等到完成再退,期间网格
    // 正常继续加载(进度条可见),完成或失败后自动续办退出。
    // 上限 20s:服务器停滞等极端情况超时后按原流程强制退出。
    for (int i = 0; i < 200 && album_ui_busy(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_task_wdt_reset(); // input_task 挂了 WDT,长等待需喂
    }
    ESP_LOGW(kTag, "prepare_stop: busy cleared, exiting");
    s_active.store(false);
    album_sync_cancel("prepare_stop");
    // 等 album_task 退出(cancel 已把网络拉取打断,通常 <100ms)。不等的话
    // 它还握着拉图缓冲的十几 KB 堆,桌面切换时两头分配一挤就崩。
    int waited = 0;
    for (int i = 0; i < 40 && s_task_handle; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    ESP_LOGW(kTag, "prepare_stop: task_exit_wait=%dms alive=%d busy=%d", waited,
             static_cast<int>(s_task_handle != nullptr),
             static_cast<int>(s_panel_busy.load()));
    ui_show_status(); // 网格/看图状态复位,LVGL 失效恢复(此时面板已单写者)
    return true;
}

void stop() {
    ESP_LOGI(kTag, "Stopping Album app");
    s_active.store(false);
    album_sync_cancel("stop");
    ui_show_status();
    bsp_lvgl_lock(-1);
    if (s_timer) { lv_timer_delete(s_timer); s_timer = nullptr; }
    if (s_screen) { lv_obj_delete(s_screen); s_screen = nullptr; }
    s_status_label = s_hint_label = nullptr;
    s_wifi_bars[0] = s_wifi_bars[1] = s_wifi_bars[2] = nullptr;
    s_slideshow = false;
    s_mode = static_cast<int>(UiMode::STATUS);
    bsp_lvgl_unlock();
}

// 按键处理:只改状态、发请求,不画屏不分配(面板单写者在 album_task)。
void on_key(uint8_t btn, app_btn_event_t event, uint16_t) {
    const int mode = s_mode.load();

    if (mode == static_cast<int>(UiMode::PHOTO)) {
        if (event == BTN_EVT_CLICK || (event == BTN_EVT_LONG && btn == BSP_BTN_OK)) {
            // 看图层的上一层:回网格(来源网格)或状态屏
            if (btn == BSP_BTN_OK) exit_photo_mode();
            else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
                const int next = photo_next_index(btn == BSP_BTN_UP ? -1 : 1);
                if (next >= 0) s_photo_req_index = next; // album_task 执行翻页
            }
            return;
        }
        return;
    }

    if (mode == static_cast<int>(UiMode::GRID)) {
        if (event == BTN_EVT_CLICK) {
            if (btn == BSP_BTN_UP) grid_move(-1);
            else if (btn == BSP_BTN_DOWN) grid_move(1);
            else if (btn == BSP_BTN_OK) {
                const int index = s_grid_page * kGridPerPage + s_grid_sel;
                if (index < album_sync_count()) { // 空格无操作
                    s_photo_req_from_grid = true;
                    s_photo_req_index = index;
                }
            }
        } else if (event == BTN_EVT_LONG) {
            // 列表层的上一层:状态屏(长按上/下/确定等效)。
            // 不设 album_sync_cancel:取消标志是粘滞的(只在重新拉清单时
            // 清零),会让之后重进网格的所有拉图秒失败(黑屏元凶)。
            if (btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) {
                ui_show_status(); // 面板写总闸在 ui_show_status 内等待直推收工
            }
        }
        return;
    }

    // 状态屏
    if (event == BTN_EVT_CLICK) {
        if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN || btn == BSP_BTN_OK) {
            enter_grid_mode(0, 0);
        }
    } else if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            if (album_sync_count()) { // 幻灯片:从第一张连续流览
                s_slideshow = true;
                s_last_advance_us = 0;
                s_photo_req_from_grid = false;
                s_photo_req_index = 0;
            }
        } else if (btn == BSP_BTN_OK) {
            // 状态屏的上一层:应用板块(系统层级)
            app_registry_switch_to(APP_ID_LAUNCHER);
        }
    }
}
} // namespace

extern const passport_app_t g_album_app = {
    .id = APP_ID_ALBUM,
    .name = "相册",
    .en_name = "ALBUM",
    .desc = "在线浏览家庭相册\n网格选择,全屏查看",
    .tag = "Wi-Fi",
    .theme_color = 0x9DB89A,
    .init = init,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .prepare_stop = prepare_stop,
};
