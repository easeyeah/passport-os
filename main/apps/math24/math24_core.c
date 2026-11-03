// 速算24 逻辑层实现:分数四则 + 回溯求解器 + 出题 + 选择状态机。
// 全部静态内存;LCG 与 tetris_core 同款,保证 host 单测可复现。
#include "apps/math24/math24_core.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 分数运算(已约分,den>0)
// ---------------------------------------------------------------------------

static int64_t gcd64(int64_t a, int64_t b) {
    while (b != 0) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a < 0 ? -a : a;
}

static m24_frac_t frac_norm(int64_t num, int64_t den) {
    if (den < 0) { num = -num; den = -den; }
    int64_t g = gcd64(num, den);
    if (g > 1) { num /= g; den /= g; }
    return (m24_frac_t){num, den};
}

static m24_frac_t frac_add(m24_frac_t a, m24_frac_t b) {
    return frac_norm(a.num * b.den + b.num * a.den, a.den * b.den);
}

static m24_frac_t frac_sub(m24_frac_t a, m24_frac_t b) {
    return frac_norm(a.num * b.den - b.num * a.den, a.den * b.den);
}

static m24_frac_t frac_mul(m24_frac_t a, m24_frac_t b) {
    return frac_norm(a.num * b.num, a.den * b.den);
}

// 调用方保证 b.num != 0。
static m24_frac_t frac_div(m24_frac_t a, m24_frac_t b) {
    return frac_norm(a.num * b.den, a.den * b.num);
}

static bool frac_is_int(const m24_frac_t *f) { return f->den == 1; }

static bool frac_eq24(const m24_frac_t *f) { return f->num == 24 * f->den; }

void m24_frac_label(const m24_frac_t *f, char *out, int cap) {
    if (frac_is_int(f)) {
        snprintf(out, cap, "%lld", (long long)f->num);
    } else {
        snprintf(out, cap, "%lld/%lld", (long long)f->num, (long long)f->den);
    }
}

// ---------------------------------------------------------------------------
// 游戏状态(先于求解器定义:m24_solve_current 要读本题原始四数)
// ---------------------------------------------------------------------------

#define M24_OPS_N 4

typedef struct {
    m24_frac_t pool[M24_POOL_MAX];
    int pool_n;
    m24_frac_t orig[M24_POOL_MAX];
    int orig_n;
    m24_stage_t stage;
    int cursor;   // PICK_OP: 0..3;其余:池下标
    int sel_a;    // 已选左操作数下标,-1 未选
    int sel_op;   // 已选运算符索引,-1 未选
    char trail[M24_TRAIL_MAX][M24_TRAIL_TEXT];
    int trail_n;
    struct {
        m24_frac_t pool[M24_POOL_MAX];
        int pool_n;
        int trail_n;
    } undo[M24_POOL_MAX - 1];
    int undo_n;
    uint32_t rng;
    int qcount;        // 当前第几题(1 起)
    int solved_count;  // 本局答对题数
    char trail_line[M24_TRAIL_MAX * M24_TRAIL_TEXT + 8];
} m24_state_t;

static m24_state_t s_state;

static const char kOpSymbols[M24_OPS_N][4] = {"+", "-", "×", "÷"};  // UTF-8

const char *m24_op_symbol(int op) {
    if (op < 0 || op >= M24_OPS_N) return "?";
    return kOpSymbols[op];
}

// op:0:+ 1:- 2:× 3:÷
static m24_frac_t op_apply(int op, m24_frac_t a, m24_frac_t b, bool *ok) {
    *ok = true;
    switch (op) {
    case 0: return frac_add(a, b);
    case 1: return frac_sub(a, b);
    case 2: return frac_mul(a, b);
    default:
        if (b.num == 0) { *ok = false; return (m24_frac_t){0, 1}; }
        return frac_div(a, b);
    }
}

// ---------------------------------------------------------------------------
// 求解器:任取两数按 + × -(双向) ÷(双向) 合并缩池,回溯记录第一条解法
// ---------------------------------------------------------------------------

static bool solve_rec(m24_frac_t *cur, int n, m24_step_t *steps, int depth) {
    if (n == 1) return frac_eq24(&cur[0]);
    m24_frac_t next[M24_POOL_MAX];
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const m24_frac_t a = cur[i], b = cur[j];
            // variant 0:+ 1:a-b 2:× 3:a÷b 4:b-a 5:b÷a
            static const uint8_t kVariantOp[6] = {0, 1, 2, 3, 1, 3};
            for (int variant = 0; variant < 6; ++variant) {
                m24_frac_t x = a, y = b;
                if (variant == 4 || variant == 5) { x = b; y = a; }
                bool ok;
                const m24_frac_t r = op_apply(kVariantOp[variant], x, y, &ok);
                if (!ok) continue;
                int w = 0;
                for (int k = 0; k < n; ++k) {
                    if (k == i || k == j) continue;
                    next[w++] = cur[k];
                }
                next[w++] = r;
                steps[depth] = (m24_step_t){x, y, r, kVariantOp[variant]};
                if (solve_rec(next, n - 1, steps, depth + 1)) return true;
            }
        }
    }
    return false;
}

bool m24_solve(const m24_frac_t *pool, int n, m24_step_t steps[M24_STEPS_MAX]) {
    if (n < 1 || n > M24_POOL_MAX) return false;
    m24_frac_t cur[M24_POOL_MAX];
    memcpy(cur, pool, sizeof(m24_frac_t) * (size_t)n);
    return solve_rec(cur, n, steps, 0);
}

bool m24_solve_current(m24_step_t steps[M24_STEPS_MAX], int *n) {
    if (!m24_solve(s_state.orig, s_state.orig_n, steps)) return false;
    *n = s_state.orig_n - 1;
    return true;
}

// ---------------------------------------------------------------------------
// 出题:1..13 随机抽 4 个数(允许重复),必须可解,不可解就重抽
// ---------------------------------------------------------------------------

static uint32_t next_rand(void) {
    s_state.rng = s_state.rng * 1664525u + 1013904223u;
    return s_state.rng >> 8;
}

static void gen_puzzle(m24_frac_t out[M24_POOL_MAX]) {
    m24_step_t scratch[M24_STEPS_MAX];
    for (int t = 0; t < 10000; ++t) {
        m24_frac_t cand[M24_POOL_MAX];
        for (int k = 0; k < M24_POOL_MAX; ++k) {
            const int v = (int)(next_rand() % M24_NUM_MAX) + M24_NUM_MIN;
            cand[k] = (m24_frac_t){v, 1};
        }
        if (m24_solve(cand, M24_POOL_MAX, scratch)) {
            memcpy(out, cand, sizeof(*out) * M24_POOL_MAX);
            return;
        }
    }
    // 1..13 随机四数可解率约八成,到不了这里;兜底必可解组合(1+2+3)×4。
    const int fallback[M24_POOL_MAX] = {1, 2, 3, 4};
    for (int k = 0; k < M24_POOL_MAX; ++k) out[k] = (m24_frac_t){fallback[k], 1};
}

// ---------------------------------------------------------------------------
// 状态访问
// ---------------------------------------------------------------------------

m24_stage_t m24_stage(void) { return s_state.stage; }
int m24_pool_n(void) { return s_state.pool_n; }
const m24_frac_t *m24_pool(void) { return s_state.pool; }
const m24_frac_t *m24_orig_pool(void) { return s_state.orig; }
int m24_cursor(void) { return s_state.cursor; }
int m24_sel_a(void) { return s_state.sel_a; }
int m24_sel_op(void) { return s_state.sel_op; }
int m24_qcount(void) { return s_state.qcount; }
int m24_solved_count(void) { return s_state.solved_count; }

bool m24_can_back(void) {
    switch (s_state.stage) {
    case M24_PICK_B:
    case M24_PICK_OP:
        return true;
    case M24_PICK_A:
    case M24_STUCK:
        return s_state.undo_n > 0;
    default:
        return false;
    }
}

const char *m24_trail_line(void) {
    if (s_state.trail_n == 0) return "";
    size_t left = sizeof(s_state.trail_line);
    char *p = s_state.trail_line;
    for (int i = 0; i < s_state.trail_n; ++i) {
        if (i > 0) {
            const int sep = snprintf(p, left, " → ");
            p += sep;
            left -= (size_t)sep;
        }
        const int w = snprintf(p, left, "%s", s_state.trail[i]);
        p += w;
        left -= (size_t)w;
    }
    return s_state.trail_line;
}

// ---------------------------------------------------------------------------
// 选择状态机
// ---------------------------------------------------------------------------

static void reset_puzzle_state(void) {
    s_state.stage = M24_PICK_A;
    s_state.cursor = 0;
    s_state.sel_a = -1;
    s_state.sel_op = -1;
    s_state.trail_n = 0;
    s_state.undo_n = 0;
}

void m24_new_game(uint32_t seed) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.rng = seed;
    m24_next_puzzle();
}

void m24_next_puzzle(void) {
    gen_puzzle(s_state.orig);
    s_state.orig_n = M24_POOL_MAX;
    memcpy(s_state.pool, s_state.orig, sizeof(s_state.pool));
    s_state.pool_n = M24_POOL_MAX;
    ++s_state.qcount;
    reset_puzzle_state();
}

void m24_load_numbers(const int vals[M24_POOL_MAX]) {
    for (int k = 0; k < M24_POOL_MAX; ++k) {
        s_state.orig[k] = (m24_frac_t){vals[k], 1};
        s_state.pool[k] = s_state.orig[k];
    }
    s_state.orig_n = M24_POOL_MAX;
    s_state.pool_n = M24_POOL_MAX;
    // 装载即开新会话:题号/答对数归零,状态可复现(测试与"每日一题"语义)。
    s_state.qcount = 1;
    s_state.solved_count = 0;
    reset_puzzle_state();
}

// 光标可选数量:PICK_OP 是 4 个运算符,其余是池大小。
static int cursor_count(void) {
    return s_state.stage == M24_PICK_OP ? M24_OPS_N : s_state.pool_n;
}

bool m24_move(int delta) {
    if (s_state.stage == M24_SOLVED || s_state.stage == M24_STUCK) return false;
    const int n = cursor_count();
    if (n <= 0) return false;
    int c = (s_state.cursor + delta) % n;
    if (c < 0) c += n;
    // PICK_B 光标跳过已选的左操作数。
    if (s_state.stage == M24_PICK_B) {
        for (int step = 0; step < n && c == s_state.sel_a; ++step) {
            c = (c + (delta >= 0 ? 1 : -1) + n) % n;
        }
    }
    s_state.cursor = c;
    return true;
}

static void trail_push(const m24_frac_t *a, int op, const m24_frac_t *b,
                       const m24_frac_t *r) {
    if (s_state.trail_n >= M24_TRAIL_MAX) return;
    char la[16], lb[16], lr[16];
    m24_frac_label(a, la, sizeof(la));
    m24_frac_label(b, lb, sizeof(lb));
    m24_frac_label(r, lr, sizeof(lr));
    // 精度限定让 GCC 可证明输出 ≤31;实际 1~13 组合的分数最长 8 字符,不会截断
    snprintf(s_state.trail[s_state.trail_n], M24_TRAIL_TEXT, "%.8s%.3s%.8s=%.8s", la,
             m24_op_symbol(op), lb, lr);
    ++s_state.trail_n;
}

static void finish_merge(int a_idx, int op, int b_idx) {
    m24_state_t *s = &s_state;
    // 合并前快照,供撤销。
    if (s->undo_n < M24_POOL_MAX - 1) {
        memcpy(s->undo[s->undo_n].pool, s->pool, sizeof(s->pool));
        s->undo[s->undo_n].pool_n = s->pool_n;
        s->undo[s->undo_n].trail_n = s->trail_n;
        ++s->undo_n;
    }
    bool ok;
    const m24_frac_t r = op_apply(op, s->pool[a_idx], s->pool[b_idx], &ok);
    if (!ok) return;  // ÷0:m24_confirm 已拦截,防御兜底
    trail_push(&s->pool[a_idx], op, &s->pool[b_idx], &r);
    int w = 0;
    for (int k = 0; k < s->pool_n; ++k) {
        if (k == a_idx || k == b_idx) continue;
        s->pool[w++] = s->pool[k];
    }
    s->pool[w++] = r;
    s->pool_n = w;
    s->cursor = 0;
    s->sel_a = -1;
    s->sel_op = -1;
    if (s->pool_n == 1) {
        s->stage = frac_eq24(&s->pool[0]) ? M24_SOLVED : M24_STUCK;
        if (s->stage == M24_SOLVED) ++s->solved_count;
    } else {
        s->stage = M24_PICK_A;
    }
}

bool m24_confirm(void) {
    m24_state_t *s = &s_state;
    switch (s->stage) {
    case M24_PICK_A:
        s->sel_a = s->cursor;
        s->stage = M24_PICK_OP;
        s->cursor = 0;
        return true;
    case M24_PICK_OP:
        s->sel_op = s->cursor;
        s->stage = M24_PICK_B;
        s->cursor = (s->sel_a == 0) ? 1 : 0;
        return true;
    case M24_PICK_B:
        if (s->cursor == s->sel_a) return false;
        // ÷0 提前拒绝,状态不变,让 UI 给出无效反馈。
        if (s->sel_op == 3 && s->pool[s->cursor].num == 0) return false;
        finish_merge(s->sel_a, s->sel_op, s->cursor);
        return true;
    case M24_SOLVED:
        m24_next_puzzle();
        return true;
    default:  // STUCK 只能回退
        return false;
    }
}

bool m24_back(void) {
    m24_state_t *s = &s_state;
    switch (s->stage) {
    case M24_PICK_B:
        s->stage = M24_PICK_OP;
        s->cursor = s->sel_op;
        s->sel_op = -1;
        return true;
    case M24_PICK_OP:
        s->cursor = s->sel_a;
        s->sel_a = -1;
        s->stage = M24_PICK_A;
        return true;
    case M24_PICK_A:
    case M24_STUCK:
        if (s->undo_n == 0) return false;
        --s->undo_n;
        memcpy(s->pool, s->undo[s->undo_n].pool, sizeof(s->pool));
        s->pool_n = s->undo[s->undo_n].pool_n;
        s->trail_n = s->undo[s->undo_n].trail_n;
        s->stage = M24_PICK_A;
        s->cursor = 0;
        s->sel_a = -1;
        s->sel_op = -1;
        return true;
    default:
        return false;
    }
}
