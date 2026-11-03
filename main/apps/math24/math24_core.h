// 速算24(m24_core):纯逻辑层,不碰 LVGL/NVS。
// 每题 4 个数,用 +-×÷ 两两合并(每个数恰好用一次)算出 24。
// 中间值可能是分数(如 8÷(3-8÷3)=24),所以全程用 int64 分数精确运算。
// 内存:状态单实例静态,游戏过程零堆分配;随机用 LCG,同 seed 序列可复现(host 单测)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M24_POOL_MAX 4
#define M24_STEPS_MAX 3      /* 4 个数合并到 1 个,恰好 3 步 */
#define M24_NUM_MIN 1
#define M24_NUM_MAX 13
#define M24_TRAIL_MAX 3      /* 每题最多 3 条玩家步骤 */
#define M24_TRAIL_TEXT 32    /* 单条步骤文本上限(含终结符) */

// 精确分数(已约分,den>0)。
typedef struct {
    int64_t num;
    int64_t den;
} m24_frac_t;

// 一条解法步骤:a op b = r。op 是运算符索引,见 m24_op_symbol()。
typedef struct {
    m24_frac_t a, b, r;
    uint8_t op;              /* 0:+ 1:- 2:× 3:÷ */
} m24_step_t;

// 题内子状态:PICK_A 选左操作数 → PICK_OP 选运算符 → PICK_B 选右操作数 →
// 自动合并;池剩 1 个时 SOLVED(=24)或 STUCK(≠24,等撤销)。
typedef enum {
    M24_PICK_A = 0,
    M24_PICK_OP,
    M24_PICK_B,
    M24_SOLVED,
    M24_STUCK,
} m24_stage_t;

// 开新的一局:重置题号/答对数,seed 出第一题(喂 esp_timer_get_time())。
void m24_new_game(uint32_t seed);

// 换下一题(答对庆祝后或菜单"换一题"):重新出题,题号 +1。
void m24_next_puzzle(void);

// 直接装载一题(测试脚本固定题目用);数字任意整数,不要求可解。
void m24_load_numbers(const int vals[M24_POOL_MAX]);

// 移动光标(+1/-1,循环)。PICK_B 自动跳过已选的左操作数;SOLVED/STUCK 无效。
bool m24_move(int delta);

// 确认:推进选择或执行合并。非法操作(如 ÷0)返回 false 且状态不变;
// SOLVED 状态下确认等价于换下一题。
bool m24_confirm(void);

// 长按下键的"回退":PICK_B→回到运算符,PICK_OP→取消左操作数,
// PICK_A→撤销最近一次合并。无可回退返回 false。
bool m24_back(void);

// 原始四个数的完整解法(菜单"显示答案"用,与玩家已走步骤无关)。
bool m24_solve_current(m24_step_t steps[M24_STEPS_MAX], int *n);

// 独立求解器:pool(n≤4)能否算出 24,能则回填第一条解法。供出题与测试用。
bool m24_solve(const m24_frac_t *pool, int n, m24_step_t steps[M24_STEPS_MAX]);

m24_stage_t m24_stage(void);
int m24_pool_n(void);
const m24_frac_t *m24_pool(void);
const m24_frac_t *m24_orig_pool(void);  // 本题原始四数(显示答案/出题回显)
int m24_cursor(void);                   // PICK_OP 时 0..3,其余为池下标
int m24_sel_a(void);                    // 已选左操作数下标,未选为 -1
int m24_sel_op(void);                   // 已选运算符索引,未选为 -1
int m24_qcount(void);                   // 当前第几题(1 起)
int m24_solved_count(void);             // 本局答对题数
bool m24_can_back(void);                // 是否有可回退内容(footer 提示用)

// 玩家已走步骤拼成的一行文本,如 "3+5=8 → 8×3=24";无步骤返回空串。
const char *m24_trail_line(void);

// 运算符显示字符(UTF-8:"×"/"÷" 为多字节)。
const char *m24_op_symbol(int op);

// 分数显示:"8"、"-5" 或 "8/3"。
void m24_frac_label(const m24_frac_t *f, char *out, int cap);

#ifdef __cplusplus
}
#endif
