// 速算24 逻辑层 host 测试:仿 test_tetris.c,直接编进产品源文件。
// 覆盖:求解器(含纯分数题/无解题)、步骤算术自洽、出题可解性与同 seed
// 可复现、选择状态机(含 PICK_B 跳过左操作数)、撤销栈、STUCK/SOLVED
// 判定、÷0 拒绝、步骤文本。
#include "apps/math24/math24_core.h"
#include <assert.h>
#include <string.h>

static m24_frac_t mk(int64_t num) { return (m24_frac_t){num, 1}; }

// 值相等(交叉相乘),供回放比对。
static bool feq(m24_frac_t a, m24_frac_t b) {
    return a.num * b.den == b.num * a.den;
}

// 独立实现四则(直接乘开不约分),交叉验证 core 解出的每一步。
static m24_frac_t eval_op(uint8_t op, m24_frac_t a, m24_frac_t b) {
    int64_t n, d;
    switch (op) {
    case 0: n = a.num * b.den + b.num * a.den; d = a.den * b.den; break;
    case 1: n = a.num * b.den - b.num * a.den; d = a.den * b.den; break;
    case 2: n = a.num * b.num; d = a.den * b.den; break;
    default: n = a.num * b.den; d = a.den * b.num; break;
    }
    return (m24_frac_t){n, d};
}

static void check_steps_arith(const m24_step_t *steps, int n) {
    for (int i = 0; i < n; ++i) {
        assert(steps[i].op <= 3);
        const m24_frac_t r = eval_op(steps[i].op, steps[i].a, steps[i].b);
        assert(feq(r, steps[i].r));
        assert(steps[i].r.den > 0);  // 已约分且分母为正
    }
}

static void check_solver_known(void) {
    m24_step_t steps[M24_STEPS_MAX];

    // 1,3,4,6 → 6÷(1-3÷4)=24,必须分数中转。
    const m24_frac_t p1[M24_POOL_MAX] = {mk(1), mk(3), mk(4), mk(6)};
    assert(m24_solve(p1, M24_POOL_MAX, steps));
    check_steps_arith(steps, M24_STEPS_MAX);

    // 3,3,8,8 → 8÷(3-8÷3)=24,同为分数题。
    const m24_frac_t p2[M24_POOL_MAX] = {mk(3), mk(3), mk(8), mk(8)};
    assert(m24_solve(p2, M24_POOL_MAX, steps));
    check_steps_arith(steps, M24_STEPS_MAX);

    // 6,6,6,6 → (6+6)+(6+6)=24,整数路径。
    const m24_frac_t p3[M24_POOL_MAX] = {mk(6), mk(6), mk(6), mk(6)};
    assert(m24_solve(p3, M24_POOL_MAX, steps));
    check_steps_arith(steps, M24_STEPS_MAX);

    // 1,1,1,1 无解。
    const m24_frac_t p4[M24_POOL_MAX] = {mk(1), mk(1), mk(1), mk(1)};
    assert(!m24_solve(p4, M24_POOL_MAX, steps));

    // 两数池:6,4 → 6×4。
    const m24_frac_t p5[2] = {mk(6), mk(4)};
    assert(m24_solve(p5, 2, steps));
    assert(steps[0].op == 2);

    // 未开局(空状态)不应有"当前题"可解。
    int n = -1;
    assert(!m24_solve_current(steps, &n));
}

static void check_generation(void) {
    m24_step_t steps[M24_STEPS_MAX];
    for (uint32_t seed = 1; seed <= 200; ++seed) {
        m24_new_game(seed);
        assert(m24_qcount() == 1);
        assert(m24_stage() == M24_PICK_A);
        assert(m24_pool_n() == M24_POOL_MAX);
        assert(m24_sel_a() == -1 && m24_sel_op() == -1);
        assert(strlen(m24_trail_line()) == 0);
        assert(!m24_can_back());
        const m24_frac_t *orig = m24_orig_pool();
        for (int k = 0; k < M24_POOL_MAX; ++k) {
            assert(orig[k].den == 1);
            assert(orig[k].num >= M24_NUM_MIN && orig[k].num <= M24_NUM_MAX);
        }
        assert(m24_solve(orig, M24_POOL_MAX, steps));  // 出的题一定可解
    }
    // 同 seed 两局,前 3 题完全一致(RNG 可复现)。
    m24_frac_t seen[3][M24_POOL_MAX];
    m24_new_game(42);
    for (int q = 0; q < 3; ++q) {
        memcpy(seen[q], m24_orig_pool(), sizeof(seen[q]));
        m24_next_puzzle();
        assert(m24_qcount() == q + 2);
    }
    m24_new_game(42);
    for (int q = 0; q < 3; ++q) {
        assert(memcmp(seen[q], m24_orig_pool(), sizeof(seen[q])) == 0);
        m24_next_puzzle();
    }
}

// 装载 1,3,4,6,按 3÷4 → 1-3/4 → 6÷(1/4) 一路通关(纯分数路径)。
static void play_fraction_win(void) {
    const int nums[M24_POOL_MAX] = {1, 3, 4, 6};
    m24_load_numbers(nums);
    assert(m24_qcount() == 1);

    // 选 3(下标 1)做左操作数。
    assert(m24_move(1));                      // 光标 0→1
    assert(m24_cursor() == 1);
    assert(m24_confirm());                    // sel_a=1 → PICK_OP
    assert(m24_stage() == M24_PICK_OP);
    assert(m24_sel_a() == 1);

    // 选 ÷(索引 3):0 左移一格绕回 3。
    assert(m24_move(-1));
    assert(m24_cursor() == 3);
    assert(m24_confirm());                    // sel_op=3 → PICK_B
    assert(m24_stage() == M24_PICK_B);
    assert(m24_cursor() != m24_sel_a());      // 光标自动避开左操作数

    // 选 4(下标 2):光标 0→1 跳过 sel_a=1 → 2。
    assert(m24_move(1));
    assert(m24_cursor() == 2);
    assert(m24_confirm());                    // 3÷4=3/4,池 {1,6,3/4}
    assert(m24_pool_n() == 3 && m24_stage() == M24_PICK_A);
    assert(strcmp(m24_trail_line(), "3÷4=3/4") == 0);

    // 选 1(下标 0),选 -,再选 3/4(下标 2):1-3/4=1/4,池 {6,1/4}。
    assert(m24_confirm());                    // sel_a=0
    assert(m24_move(1) && m24_cursor() == 1); // op '-'(1)
    assert(m24_confirm());                    // PICK_B,光标让开 0 → 1(6)
    assert(m24_cursor() == 1);
    assert(m24_move(1) && m24_cursor() == 2);
    assert(m24_confirm());                    // 池 {6,1/4}
    assert(m24_pool_n() == 2 && m24_stage() == M24_PICK_A);
    assert(strcmp(m24_trail_line(), "3÷4=3/4 → 1-3/4=1/4") == 0);

    // 选 6,选 ÷,选 1/4:6÷(1/4)=24,通关。
    assert(m24_confirm());                    // sel_a=0(6)
    assert(m24_move(-1) && m24_cursor() == 3); // op ÷(3)
    assert(m24_confirm());                    // PICK_B,光标 → 1(1/4)
    assert(m24_cursor() == 1);
    assert(m24_confirm());                    // 合并完成
    assert(m24_stage() == M24_SOLVED);
    assert(m24_pool_n() == 1);
    assert(m24_solved_count() == 1);
    assert(m24_pool()[0].num == 24 && m24_pool()[0].den == 1);
    assert(strcmp(m24_trail_line(),
                  "3÷4=3/4 → 1-3/4=1/4 → 6÷1/4=24") == 0);

    // SOLVED:移动/回退停用;确认即换下一题。
    assert(!m24_move(1) && !m24_back());
    m24_step_t steps[M24_STEPS_MAX];
    int n = 0;
    assert(m24_solve_current(steps, &n) && n == M24_STEPS_MAX);
    check_steps_arith(steps, n);
    assert(m24_confirm());
    assert(m24_stage() == M24_PICK_A && m24_pool_n() == M24_POOL_MAX);
    assert(m24_qcount() == 2);
    assert(m24_solved_count() == 1);
    assert(strlen(m24_trail_line()) == 0);
}

// 1,1,1,1:算出 4 ≠ 24,验证 STUCK 与逐级撤销。
static void check_stuck_and_undo(void) {
    const int nums[M24_POOL_MAX] = {1, 1, 1, 1};
    m24_load_numbers(nums);

    // 1+1=2(选 0、'+'、选 1)→ 池 {1,1,2}。
    assert(m24_confirm() && m24_confirm() && m24_confirm());
    assert(m24_pool_n() == 3);
    // 2+1=3:光标移到 2(下标 2)做左操作数。
    assert(m24_move(1) && m24_move(1));
    assert(m24_cursor() == 2);
    assert(m24_confirm());                    // sel_a=2
    assert(m24_confirm());                    // op '+',PICK_B 光标 → 0(1)
    assert(m24_confirm());                    // 2+1=3 → 池 {1,3}
    assert(m24_pool_n() == 2);
    // 1+3=4。
    assert(m24_confirm());                    // sel_a=0(1)
    assert(m24_confirm());                    // op '+',PICK_B 光标 → 1(3)
    assert(m24_confirm());                    // 1+3=4 → 池 {4}
    assert(m24_stage() == M24_STUCK);
    assert(m24_pool()[0].num == 4 && m24_pool()[0].den == 1);
    assert(!m24_move(1) && !m24_confirm());
    assert(m24_can_back());
    assert(strcmp(m24_trail_line(), "1+1=2 → 2+1=3 → 1+3=4") == 0);

    // 连撤三步回到开局:撤销合并 → 撤销选择 → 撤销选择。
    assert(m24_back());  // 回到 {1,3},PICK_A
    assert(m24_stage() == M24_PICK_A && m24_pool_n() == 2);
    assert(m24_back());  // 回到 {1,1,2},PICK_A
    assert(m24_pool_n() == 3);
    assert(strcmp(m24_trail_line(), "1+1=2") == 0);
    // 从 {1,1,2} 再连撤两次合并到开局。
    assert(m24_back());
    assert(m24_pool_n() == M24_POOL_MAX);
    assert(strlen(m24_trail_line()) == 0);
    assert(!m24_back());  // 无可再撤

    // 选择过程的逐级回退:选 A → 选 op → back → back。
    assert(m24_confirm());                    // sel_a=0
    assert(m24_confirm());                    // op '+'(0)
    assert(m24_stage() == M24_PICK_B);
    assert(m24_back());                       // 回到 PICK_OP
    assert(m24_stage() == M24_PICK_OP && m24_sel_op() == -1);
    assert(m24_back());                       // 回到 PICK_A
    assert(m24_stage() == M24_PICK_A && m24_sel_a() == -1);
}

// 装载 1,1,2,6,先合并出 0,再验证 ÷0 被拒绝且状态不变。
static void check_div_by_zero_rejected(void) {
    const int nums[M24_POOL_MAX] = {1, 1, 2, 6};
    m24_load_numbers(nums);

    // 1-1=0(选 0、'-'、选 1)→ 池 {2,6,0}。
    assert(m24_confirm());                    // sel_a=0
    assert(m24_move(1) && m24_cursor() == 1); // op '-'(1)
    assert(m24_confirm());                    // PICK_B,光标让开 → 1? sel_a=0 → 1
    assert(m24_cursor() == 1);
    assert(m24_confirm());                    // 1-1=0,池 {2,6,0}
    assert(m24_pool_n() == 3);

    // 2÷0:选 2(下标 0)→ ÷ → 光标让开 sel_a → 1(6),再移到 2(0)确认。
    assert(m24_confirm());                    // sel_a=0(2)
    assert(m24_move(-1) && m24_cursor() == 3); // op ÷
    assert(m24_confirm());                    // PICK_B,光标 → 1(6)
    assert(m24_move(1) && m24_cursor() == 2); // 移到 0
    const char *trail_before = m24_trail_line();
    assert(!m24_confirm());                   // ÷0 被拒绝
    assert(m24_stage() == M24_PICK_B);
    assert(strcmp(m24_trail_line(), trail_before) == 0);
    assert(m24_pool_n() == 3);

    // 无解题:看答案直接失败,不崩溃。
    const int bad[M24_POOL_MAX] = {1, 1, 1, 1};
    m24_load_numbers(bad);
    m24_step_t steps[M24_STEPS_MAX];
    int n = 0;
    assert(!m24_solve_current(steps, &n));
}

static void check_labels(void) {
    char buf[24];
    const m24_frac_t i24 = mk(24);
    m24_frac_label(&i24, buf, sizeof(buf));
    assert(strcmp(buf, "24") == 0);
    const m24_frac_t neg = mk(-5);
    m24_frac_label(&neg, buf, sizeof(buf));
    assert(strcmp(buf, "-5") == 0);
    const m24_frac_t f = {8, 3};
    m24_frac_label(&f, buf, sizeof(buf));
    assert(strcmp(buf, "8/3") == 0);
    const m24_frac_t red = {26, 4};  // 显示前应已由 core 约分为 13/2
    (void)red;
    assert(strcmp(m24_op_symbol(0), "+") == 0);
    assert(strcmp(m24_op_symbol(1), "-") == 0);
    assert(strcmp(m24_op_symbol(2), "×") == 0);
    assert(strcmp(m24_op_symbol(3), "÷") == 0);
}

int main(void) {
    check_solver_known();
    check_generation();
    play_fraction_win();
    check_stuck_and_undo();
    check_div_by_zero_rejected();
    check_labels();
    return 0;
}
