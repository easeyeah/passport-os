// 速算24 UI 壳:3 键玩 24 点。数字卡片 + 运算符行 + 算式历史行,
// 长按上呼出菜单(继续/看答案/换一题),长按下逐步回退。
// 生命周期与渲染约束同 app_tetris:input 任务栈小,start() 只建 50/100ms
// lv_timer,UI 推迟到首帧 tick 在 LVGL 任务里建;on_key 里 lv_* 必须持锁。
// 本应用无倒计时无计分:tick 只做电量刷新与答对庆祝的自动下一题。
#include "apps/math24/app_math24.h"

#include "apps/math24/math24_core.h"
#include "bsp_display.h"
#include "bsp_battery.h"
#include "launcher/app_registry.h"
#include "ui_pixel.h"
#include "fonts/app_fonts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lvgl.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "app_math24";
constexpr uint32_t kTickMs = 100;
constexpr int kCardW = 48;
constexpr int kCardH = 64;
constexpr int kCardGap = 8;
constexpr int kContentW = 216;           // 240 屏的安全区宽度
constexpr int kOpsN = 4;
constexpr int kMenuItems = 3;
constexpr int kCelebrateTicks = 14;      // 答对庆祝 1.4s 后自动下一题

std::atomic<bool> s_active{false};
lv_timer_t *s_timer = nullptr;
lv_obj_t *s_screen = nullptr;
ui_pixel_battery_t s_battery{};
int64_t s_battery_us = 0;

lv_obj_t *s_q_lbl = nullptr;       // "第 N 题"
lv_obj_t *s_ok_lbl = nullptr;      // "对 M 题"
lv_obj_t *s_trail_lbl = nullptr;   // 算式历史行
lv_obj_t *s_cards[M24_POOL_MAX] = {};
lv_obj_t *s_card_lbls[M24_POOL_MAX] = {};
lv_obj_t *s_ops[kOpsN] = {};
lv_obj_t *s_op_lbls[kOpsN] = {};
lv_obj_t *s_footer_lbl = nullptr;

enum class Overlay { None, Menu, Answer, Win };
Overlay s_overlay = Overlay::None;
lv_obj_t *s_menu_panel = nullptr;
lv_obj_t *s_menu_items[kMenuItems] = {};
lv_obj_t *s_answer_panel = nullptr;
lv_obj_t *s_answer_body = nullptr;
lv_obj_t *s_win_panel = nullptr;
int s_menu_sel = 0;
int s_celebrate = 0;

// 差分缓存:按键驱动的重绘频率低,但避免无谓的 LVGL 失效区域。
int s_pool_n_cache = -1;
int s_stage_cache = -1;
int s_cursor_cache = -1;
int s_sel_a_cache = -1;
int s_sel_op_cache = -1;
int s_q_cache = -1;
int s_ok_cache = -1;
char s_trail_cache[M24_TRAIL_MAX * M24_TRAIL_TEXT + 8] = "";

lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   uint32_t color, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

// 卡片/运算符钮:同一套像素卡片语言(纸底 + 描边 + 方角)。
lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    if (!o) return nullptr;
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    ui_pixel_set_selected(o, false, true);
    lv_obj_set_style_border_width(o, 2, 0);
    return o;
}

lv_obj_t *mk_panel(int x, int y, int w, int h) {
    lv_obj_t *p = mk_card(s_screen, x, y, w, h);
    if (p) lv_obj_set_style_radius(p, 6, 0);
    return p;
}

void park(lv_obj_t *panel) {
    if (!panel) return;
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(panel, 344);  // 移出屏幕(与 tetris 浮层同款)
}

void place(lv_obj_t *panel, int y) {
    if (!panel) return;
    lv_obj_set_y(panel, y);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
}

// 卡片三种视觉态:普通 / 光标(琥珀描边) / 已选(琥珀底)。
enum CardState { CARD_PLAIN = 0, CARD_CURSOR, CARD_PICKED };

void card_apply(lv_obj_t *card, lv_obj_t *lbl, CardState state, const char *text) {
    if (!card || !lbl) return;
    lv_obj_set_style_bg_color(card,
        lv_color_hex(state == CARD_PICKED ? UI_AMBER
                     : state == CARD_CURSOR ? UI_SURFACE_ALT : UI_SURFACE), 0);
    lv_obj_set_style_border_color(card,
        lv_color_hex(state == CARD_PLAIN ? UI_BORDER : UI_AMBER), 0);
    lv_obj_set_style_text_color(lbl,
        lv_color_hex(state == CARD_PICKED ? UI_INK : UI_TEXT), 0);
    if (text && (!lv_label_get_text(lbl) || strcmp(lv_label_get_text(lbl), text) != 0)) {
        // 中间值可能是 "2197" 这类 4 位结果,卡宽塞不下就降到 14 号。
        const lv_font_t *font = strlen(text) > 3 ? &lv_font_montserrat_14
                                                 : &lv_font_montserrat_20;
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_label_set_text(lbl, text);
    }
}

const char *stage_action(m24_stage_t stage) {
    switch (stage) {
    case M24_PICK_A: return "选一个数字";
    case M24_PICK_OP: return "选一个运算符";
    case M24_PICK_B: return "再选一个数字";
    case M24_STUCK: return "不是24 长按下撤销";
    case M24_SOLVED: return "算对了!";
    default: return "";
    }
}

void refresh_menu_items() {
    for (int i = 0; i < kMenuItems; ++i) {
        if (!s_menu_items[i]) continue;
        lv_obj_set_style_text_color(s_menu_items[i],
            lv_color_hex(i == s_menu_sel ? UI_AMBER : UI_TEXT), 0);
    }
}

void render() {
    if (!s_screen) return;
    const m24_stage_t stage = m24_stage();
    const int pool_n = m24_pool_n();
    const int cursor = m24_cursor();
    const int sel_a = m24_sel_a();
    const int sel_op = m24_sel_op();

    if (m24_qcount() != s_q_cache) {
        s_q_cache = m24_qcount();
        char text[24];
        snprintf(text, sizeof(text), "第 %d 题", s_q_cache);
        lv_label_set_text(s_q_lbl, text);
    }
    if (m24_solved_count() != s_ok_cache) {
        s_ok_cache = m24_solved_count();
        char text[24];
        snprintf(text, sizeof(text), "对 %d 题", s_ok_cache);
        lv_label_set_text(s_ok_lbl, text);
    }
    const char *trail = m24_trail_line();
    if (strcmp(trail, s_trail_cache) != 0) {
        snprintf(s_trail_cache, sizeof(s_trail_cache), "%s", trail);
        lv_label_set_text(s_trail_lbl, s_trail_cache);
    }
    if ((int)stage != s_stage_cache) {
        s_stage_cache = (int)stage;
        lv_label_set_text(s_footer_lbl, stage_action(stage));
    }

    // 数字卡片:池缩小时行居中;池内容或选择态变化都要重绘文本与高亮。
    bool pool_changed = false;
    if (pool_n != s_pool_n_cache) {
        pool_changed = true;
        s_pool_n_cache = pool_n;
        const int total = pool_n * kCardW + (pool_n - 1) * kCardGap;
        const int x0 = 12 + (kContentW - total) / 2;
        for (int i = 0; i < M24_POOL_MAX; ++i) {
            if (!s_cards[i]) continue;
            if (i < pool_n) {
                lv_obj_set_x(s_cards[i], x0 + i * (kCardW + kCardGap));
                lv_obj_remove_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_cards[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    const bool sel_changed = cursor != s_cursor_cache || sel_a != s_sel_a_cache ||
                             sel_op != s_sel_op_cache;
    if (pool_changed || sel_changed) {
        s_cursor_cache = cursor;
        s_sel_a_cache = sel_a;
        s_sel_op_cache = sel_op;
        for (int i = 0; i < pool_n; ++i) {
            if (!s_cards[i] || !s_card_lbls[i]) continue;
            CardState st = CARD_PLAIN;
            if (stage != M24_SOLVED && stage != M24_STUCK && i == cursor) st = CARD_CURSOR;
            if (i == sel_a) st = CARD_PICKED;
            char text[24];
            m24_frac_label(&m24_pool()[i], text, sizeof(text));
            card_apply(s_cards[i], s_card_lbls[i], st, text);
        }
        for (int i = 0; i < kOpsN; ++i) {
            if (!s_ops[i] || !s_op_lbls[i]) continue;
            CardState st = CARD_PLAIN;
            if (stage == M24_PICK_OP && i == cursor) st = CARD_CURSOR;
            if (stage == M24_PICK_B && i == sel_op) st = CARD_PICKED;
            card_apply(s_ops[i], s_op_lbls[i], st, m24_op_symbol(i));
        }
    }
}

// 菜单"显示答案":把原始四数的第一条解法排成 ①②③ 步骤。
void open_answer() {
    m24_step_t steps[M24_STEPS_MAX];
    int n = 0;
    char body[160];
    if (!m24_solve_current(steps, &n)) {
        snprintf(body, sizeof(body), "这道题算不出 24");
    } else {
        static const char *kCircled[M24_STEPS_MAX] = {"①", "②", "③"};
        int off = 0;
        size_t left = sizeof(body);
        for (int i = 0; i < n; ++i) {
            char la[16], lb[16], lr[16];
            m24_frac_label(&steps[i].a, la, sizeof(la));
            m24_frac_label(&steps[i].b, lb, sizeof(lb));
            m24_frac_label(&steps[i].r, lr, sizeof(lr));
            const int w = snprintf(body + off, left, "%s %s%s%s=%s\n",
                                   kCircled[i], la, m24_op_symbol(steps[i].op), lb, lr);
            off += w;
            left -= (size_t)w;
        }
    }
    lv_label_set_text(s_answer_body, body);
    s_overlay = Overlay::Answer;
    place(s_answer_panel, 70);
}

void open_menu() {
    s_menu_sel = 0;
    refresh_menu_items();
    s_overlay = Overlay::Menu;
    place(s_menu_panel, 84);
}

void close_overlays() {
    s_overlay = Overlay::None;
    park(s_menu_panel);
    park(s_answer_panel);
}

void show_win() {
    s_celebrate = 0;
    s_overlay = Overlay::Win;
    place(s_win_panel, 100);
}

void next_puzzle_ui() {
    m24_next_puzzle();
    close_overlays();
    render();
}

void build_ui() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    ui_pixel_background(s_screen);

    lv_obj_t *bar = ui_pixel_top_bar(s_screen);
    lv_obj_t *title = mk_label(bar, "速算24", &buddy_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT);
    ui_pixel_status_text_align(title, 8, 84, LV_TEXT_ALIGN_LEFT);
    s_battery = ui_pixel_battery_create(bar, 206, 6, UI_TEXT, &lv_font_montserrat_14);
    ui_pixel_battery_set(&s_battery, bsp_battery_soc());
    s_battery_us = esp_timer_get_time();

    s_q_lbl = mk_label(s_screen, "", &buddy_font_16, UI_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_pos(s_q_lbl, 12, 36);
    s_ok_lbl = mk_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM, LV_TEXT_ALIGN_RIGHT);
    lv_obj_set_pos(s_ok_lbl, 120, 36);
    lv_obj_set_width(s_ok_lbl, 108);
    s_trail_lbl = mk_label(s_screen, "", &buddy_font_16, UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_pos(s_trail_lbl, 12, 60);
    lv_obj_set_width(s_trail_lbl, kContentW);
    lv_label_set_long_mode(s_trail_lbl, LV_LABEL_LONG_MODE_DOTS);

    for (int i = 0; i < M24_POOL_MAX; ++i) {
        s_cards[i] = mk_card(s_screen, 12 + i * (kCardW + kCardGap), 88, kCardW, kCardH);
        if (!s_cards[i]) continue;
        s_card_lbls[i] = mk_label(s_cards[i], "", &lv_font_montserrat_20, UI_TEXT,
                                  LV_TEXT_ALIGN_CENTER);
        lv_label_set_long_mode(s_card_lbls[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(s_card_lbls[i], kCardW);
        lv_obj_center(s_card_lbls[i]);
    }
    for (int i = 0; i < kOpsN; ++i) {
        s_ops[i] = mk_card(s_screen, 12 + i * (kCardW + kCardGap), 168, kCardW, 40);
        if (!s_ops[i]) continue;
        s_op_lbls[i] = mk_label(s_ops[i], m24_op_symbol(i), &buddy_font_16, UI_TEXT,
                                LV_TEXT_ALIGN_CENTER);
        lv_obj_center(s_op_lbls[i]);
    }
    s_footer_lbl = ui_pixel_footer(s_screen, "", "长按上:菜单  长按下:撤销",
                                   &buddy_font_16);

    // 菜单浮层(屏外预建)。
    s_menu_panel = mk_panel(12, 344, kContentW, 152);
    lv_obj_t *mt = mk_label(s_menu_panel, "暂停菜单", &buddy_font_16, UI_TEXT,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(mt, kContentW - 8);
    lv_obj_set_pos(mt, 4, 12);
    static const char *kMenuTexts[kMenuItems] = {"继续做题", "显示答案", "换一题"};
    for (int i = 0; i < kMenuItems; ++i) {
        s_menu_items[i] = mk_label(s_menu_panel, kMenuTexts[i], &buddy_font_16,
                                   UI_TEXT, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_width(s_menu_items[i], kContentW - 16);
        lv_obj_set_pos(s_menu_items[i], 8, 44 + i * 34);
    }
    // 答案浮层。
    s_answer_panel = mk_panel(12, 344, kContentW, 196);
    lv_obj_t *at = mk_label(s_answer_panel, "参考答案", &buddy_font_16, UI_TEXT,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(at, kContentW - 8);
    lv_obj_set_pos(at, 4, 12);
    s_answer_body = mk_label(s_answer_panel, "", &buddy_font_16, UI_TEXT,
                             LV_TEXT_ALIGN_LEFT);
    lv_obj_set_width(s_answer_body, kContentW - 32);
    lv_obj_set_pos(s_answer_body, 16, 44);
    lv_obj_set_style_text_line_space(s_answer_body, 8, 0);
    lv_obj_t *asub = mk_label(s_answer_panel, "确定:下一题  长按上:返回", &buddy_font_16,
                              UI_TEXT_DIM, LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(asub, kContentW - 16);
    lv_obj_set_pos(asub, 8, 156);
    // 答对庆祝浮层。
    s_win_panel = mk_panel(36, 344, 168, 96);
    lv_obj_set_style_border_color(s_win_panel, lv_color_hex(UI_SUCCESS), 0);
    lv_obj_t *wt = mk_label(s_win_panel, "算对了!", &buddy_font_16, UI_SUCCESS,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(wt, 160);
    lv_obj_set_pos(wt, 4, 16);
    lv_obj_t *ws = mk_label(s_win_panel, "= 24", &lv_font_montserrat_20, UI_TEXT,
                            LV_TEXT_ALIGN_CENTER);
    lv_obj_set_width(ws, 160);
    lv_obj_set_pos(ws, 4, 48);

    park(s_menu_panel);
    park(s_answer_panel);
    park(s_win_panel);
    s_overlay = Overlay::None;
    render();

    ESP_LOGI(kTag, "ui ready; free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void tick(lv_timer_t *) {
    if (!s_active.load()) return;
    if (!s_screen) {
        build_ui();
        lv_screen_load(s_screen);
        return;
    }
    const bool visible = app_registry_screen_is_on();
    if (s_timer) lv_timer_set_period(s_timer, visible ? kTickMs : 1000);
    if (!visible) return;
    const int64_t now = esp_timer_get_time();
    if (now - s_battery_us >= 5000000) {
        ui_pixel_battery_set(&s_battery, bsp_battery_soc());
        s_battery_us = now;
    }
    if (s_overlay == Overlay::Win && ++s_celebrate >= kCelebrateTicks) {
        next_puzzle_ui();
    }
}

}  // namespace

extern "C" {

static void math24_init(void) {
    // 核心状态跨应用切换保留;首次进入才开局。
    if (m24_qcount() == 0) {
        m24_new_game((uint32_t)esp_timer_get_time() & 0x7fffffffu);
    }
}

static void math24_start(void) {
    if (s_active.load()) return;
    s_active.store(true);
    bsp_lvgl_lock(-1);
    s_timer = lv_timer_create(tick, kTickMs, nullptr);  // UI 由首帧 tick 建
    bsp_lvgl_unlock();
}

static void math24_stop(void) {
    if (!s_active.load()) return;
    s_active.store(false);
    if (s_timer) {  // 先删定时器,防止 tick 撕屏途中再进来
        bsp_lvgl_lock(-1);
        lv_timer_del(s_timer);
        s_timer = nullptr;
        bsp_lvgl_unlock();
    }
    bsp_lvgl_lock(-1);
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = nullptr;
    }
    bsp_lvgl_unlock();
    s_q_lbl = s_ok_lbl = s_trail_lbl = s_footer_lbl = nullptr;
    for (lv_obj_t *&c : s_cards) c = nullptr;
    for (lv_obj_t *&c : s_card_lbls) c = nullptr;
    for (lv_obj_t *&o : s_ops) o = nullptr;
    for (lv_obj_t *&o : s_op_lbls) o = nullptr;
    for (lv_obj_t *&m : s_menu_items) m = nullptr;
    s_menu_panel = s_answer_panel = s_answer_body = s_win_panel = nullptr;
    s_overlay = Overlay::None;
    s_menu_sel = 0;
    s_celebrate = 0;
    s_pool_n_cache = s_stage_cache = s_cursor_cache = -1;
    s_sel_a_cache = s_sel_op_cache = s_q_cache = s_ok_cache = -1;
    s_trail_cache[0] = '\0';
}

static void math24_on_key(uint8_t btn, app_btn_event_t event, uint16_t mv) {
    (void)mv;
    if (!s_active.load() || !s_screen) return;  // 首帧 UI 未建前忽略按键

    // 本回调跑在输入任务:lv_* 调用必须持锁,否则与渲染任务竞态(真机踩过)。
    if (!bsp_lvgl_lock(-1)) return;
    if (s_timer) lv_timer_set_period(s_timer, kTickMs);

    if (event == BTN_EVT_LONG) {
        if (btn == BSP_BTN_UP) {
            if (s_overlay == Overlay::None && m24_stage() != M24_SOLVED) {
                open_menu();
            } else if (s_overlay == Overlay::Menu || s_overlay == Overlay::Answer) {
                close_overlays();
                render();
            }
        } else if (btn == BSP_BTN_DOWN) {
            if (s_overlay == Overlay::None) {
                if (m24_back()) {
                    render();
                } else {
                    ui_pixel_focus_feedback(s_cards[m24_cursor()]);
                }
            } else if (s_overlay == Overlay::Answer) {
                // 看完答案先放人回去继续想,不强制跳题。
                close_overlays();
                render();
            }
        }
    } else if (event == BTN_EVT_CLICK) {
        switch (s_overlay) {
        case Overlay::Menu:
            if (btn == BSP_BTN_UP) {
                s_menu_sel = (s_menu_sel + kMenuItems - 1) % kMenuItems;
                refresh_menu_items();
            } else if (btn == BSP_BTN_DOWN) {
                s_menu_sel = (s_menu_sel + 1) % kMenuItems;
                refresh_menu_items();
            } else if (btn == BSP_BTN_OK) {
                if (s_menu_sel == 0) {
                    close_overlays();
                    render();
                } else if (s_menu_sel == 1) {
                    open_answer();
                } else {
                    next_puzzle_ui();
                }
            }
            break;
        case Overlay::Answer:
            if (btn == BSP_BTN_OK) next_puzzle_ui();  // 看完答案直接下一题
            break;
        case Overlay::Win:
            if (btn == BSP_BTN_OK) next_puzzle_ui();  // 不等庆祝结束,直接下一题
            break;
        case Overlay::None:
            if (m24_stage() == M24_STUCK && btn == BSP_BTN_OK) {
                open_menu();  // 卡住时确定键也能呼出菜单
                break;
            }
            if (btn == BSP_BTN_UP) {
                if (!m24_move(-1)) ui_pixel_focus_feedback(s_cards[m24_cursor()]);
                else render();
            } else if (btn == BSP_BTN_DOWN) {
                if (!m24_move(1)) ui_pixel_focus_feedback(s_cards[m24_cursor()]);
                else render();
            } else if (btn == BSP_BTN_OK) {
                if (!m24_confirm()) {
                    // 非法操作(如 ÷0):闪一下当前目标,状态不变。
                    const int c = m24_cursor();
                    ui_pixel_focus_feedback(
                        m24_stage() == M24_PICK_OP ? s_ops[c] : s_cards[c]);
                } else {
                    render();
                    if (m24_stage() == M24_SOLVED) show_win();
                }
            }
            break;
        }
    }

    bsp_lvgl_unlock();
}

const passport_app_t g_math24_app = {
    .id = APP_ID_MATH24,
    .name = "速算24",
    .en_name = "MATH24",
    .desc = "加减乘除凑出24\n动动脑,随时来一局",
    .tag = "GAME",
    .theme_color = 0x2E7D5B,
    .init = math24_init,
    .start = math24_start,
    .stop = math24_stop,
    .on_key = math24_on_key,
};

}  // extern "C"
