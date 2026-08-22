/* VM tests: opcode semantics, bounds, scheduling, and the invariant
 * that makes save-anywhere possible. */
#include "vn_vm.h"
#include "vn_save.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) {                                               \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);            \
        printf(__VA_ARGS__); printf("\n");                       \
        g_fail++;                                                \
    }                                                            \
} while (0)

/* --- a hand-assembled program, so tests do not depend on the compiler */

static uint8_t  g_codebuf[4096];
static uint32_t g_n;
static uint8_t  g_blob[8192];
static vn_program g_prog;

static void c8(uint8_t v)  { g_codebuf[g_n++] = v; }
static void c16(uint16_t v){ vn_wr16(g_codebuf + g_n, v); g_n += 2; }
static void c32(uint32_t v){ vn_wr32(g_codebuf + g_n, v); g_n += 4; }

/* Wrap raw code in a minimal .vnb with one label at address 0. */
static void finish_program(void)
{
    uint32_t code_off = VN_VNB_HEADER_SIZE;
    uint32_t label_off = code_off + g_n;
    uint32_t sym_off = label_off + VN_LABEL_REC_SIZE;

    memset(g_blob, 0, sizeof g_blob);
    memcpy(g_blob, VN_VNB_MAGIC, 4);
    vn_wr16(g_blob + 4, VN_VNB_VERSION);
    uint8_t *h = g_blob + 8;
    vn_wr32(h + 0, vn_hash_bytes(g_codebuf, g_n));
    vn_wr32(h + 4, code_off);
    vn_wr32(h + 8, g_n);
    vn_wr32(h + 12, label_off);
    vn_wr32(h + 16, 1);
    vn_wr32(h + 20, sym_off);
    vn_wr32(h + 24, 0);
    vn_wr32(h + 28 + 4 * VN_BANK_F, 8);
    vn_wr32(h + 28 + 4 * VN_BANK_SF, 8);
    vn_wr32(h + 28 + 4 * VN_BANK_TF, 8);
    vn_wr32(h + 28 + 4 * VN_BANK_L, 8);

    memcpy(g_blob + code_off, g_codebuf, g_n);
    vn_wr32(g_blob + label_off + 0, vn_hash("main"));
    vn_wr32(g_blob + label_off + 4, 0);
    vn_wr32(g_blob + label_off + 8, vn_hash_bytes(g_codebuf, g_n));
    vn_wr16(g_blob + label_off + 12, VN_STR_NONE);

    CHECK(vn_program_load(&g_prog, g_blob, sym_off) == 1, "hand-built program failed to load");
}

static void reset(void) { g_n = 0; memset(g_codebuf, 0, sizeof g_codebuf); }

/* Run to completion and return the value left in f[0]. */
static int32_t run_expr(void)
{
    finish_program();
    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    vn_run(&m, &g_prog, &pr, 0, 10000);
    CHECK(m.error == 0, "vm trapped: %s", m.error_msg ? m.error_msg : "?");
    return m.f[0];
}

static void emit_store_f0(void) { c8(VN_OP_STORE); c8(VN_BANK_F); c16(0); }

static void test_saturating_arithmetic(void)
{
    /* INT32_MAX + 1 must clamp, not wrap. A wrapped affection counter
     * is the classic unreproducible VN bug. */
    reset();
    c8(VN_OP_PUSH); c32((uint32_t)INT32_MAX);
    c8(VN_OP_PUSH); c32(1);
    c8(VN_OP_ADD);
    emit_store_f0();
    c8(VN_OP_HALT);
    CHECK(run_expr() == INT32_MAX, "add did not clamp at INT32_MAX");

    reset();
    c8(VN_OP_PUSH); c32((uint32_t)INT32_MIN);
    c8(VN_OP_PUSH); c32(1);
    c8(VN_OP_SUB);
    emit_store_f0();
    c8(VN_OP_HALT);
    CHECK(run_expr() == INT32_MIN, "sub did not clamp at INT32_MIN");

    reset();
    c8(VN_OP_PUSH); c32(100000);
    c8(VN_OP_PUSH); c32(100000);
    c8(VN_OP_MUL);
    emit_store_f0();
    c8(VN_OP_HALT);
    CHECK(run_expr() == INT32_MAX, "mul did not clamp");
}

static void test_comparisons_and_logic(void)
{
    struct { uint8_t op; int32_t a, b, want; } cases[] = {
        { VN_OP_EQ, 3, 3, 1 }, { VN_OP_EQ, 3, 4, 0 },
        { VN_OP_NE, 3, 4, 1 }, { VN_OP_LT, 3, 4, 1 }, { VN_OP_LT, 4, 3, 0 },
        { VN_OP_LE, 4, 4, 1 }, { VN_OP_GT, 5, 4, 1 }, { VN_OP_GE, 4, 5, 0 },
        { VN_OP_AND, 1, 0, 0 }, { VN_OP_AND, 2, 3, 1 },
        { VN_OP_OR, 0, 0, 0 }, { VN_OP_OR, 0, 7, 1 }
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset();
        c8(VN_OP_PUSH); c32((uint32_t)cases[i].a);
        c8(VN_OP_PUSH); c32((uint32_t)cases[i].b);
        c8(cases[i].op);
        emit_store_f0();
        c8(VN_OP_HALT);
        CHECK(run_expr() == cases[i].want,
              "op %u(%d,%d) gave %d, want %d", cases[i].op,
              cases[i].a, cases[i].b, (int)run_expr(), cases[i].want);
    }
}

static void test_call_depth_bounded(void)
{
    /* Infinite recursion must trap cleanly, not smash the thread. */
    reset();
    c8(VN_OP_CALL); c32(0);
    finish_program();

    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    vn_run_result r = vn_run(&m, &g_prog, &pr, 0, 100000);
    CHECK(r == VN_RUN_ERROR, "unbounded recursion did not trap");
    CHECK(m.thread[0].call_depth <= VN_CALL_DEPTH, "call depth exceeded its bound");
}

static void test_expr_stack_bounded(void)
{
    reset();
    for (int i = 0; i < VN_EXPR_STACK + 4; i++) { c8(VN_OP_PUSH); c32(1); }
    c8(VN_OP_HALT);
    finish_program();

    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    CHECK(vn_run(&m, &g_prog, &pr, 0, 10000) == VN_RUN_ERROR,
          "expression stack overflow was not trapped");
    CHECK(m.thread[0].sp <= VN_EXPR_STACK, "stack pointer ran past its bound");
}

static void test_dirty_stack_is_trapped(void)
{
    /* The invariant that makes a snapshot a memcpy: a blocking opcode
     * with residue on the expression stack must be caught. The compiler
     * never emits this, so only a corrupt .vnb can reach it. */
    reset();
    c8(VN_OP_PUSH); c32(7);
    c8(VN_OP_NARR); c16(0);
    c8(VN_OP_HALT);
    finish_program();

    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    CHECK(vn_run(&m, &g_prog, &pr, 0, 1000) == VN_RUN_ERROR,
          "blocking with a dirty expression stack was not trapped");
}

static void test_bad_bytecode_is_rejected(void)
{
    reset();
    c8(200);                      /* not an opcode */
    finish_program();
    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    CHECK(vn_run(&m, &g_prog, &pr, 0, 100) == VN_RUN_ERROR, "unknown opcode accepted");

    /* An instruction whose operands run past the end of the code. */
    reset();
    c8(VN_OP_PUSH); c8(1);        /* PUSH needs 4 operand bytes, has 1 */
    finish_program();
    vn_machine_init(&m, &g_prog, 0);
    CHECK(vn_run(&m, &g_prog, &pr, 0, 100) == VN_RUN_ERROR, "truncated instruction accepted");
}

static void test_threads_and_screen_ownership(void)
{
    /* Two threads both reach a line. Only one may own the screen; the
     * other must retry rather than overwrite it. */
    reset();
    uint32_t spawn_at = g_n;
    c8(VN_OP_SPAWN); c32(0);      /* patched below */
    c8(VN_OP_NARR); c16(0);
    c8(VN_OP_END_THREAD);         /* end THIS thread; HALT would stop
                                     the whole machine and the second
                                     thread would never run - which is
                                     correct, but not what we test here */
    uint32_t second = g_n;
    c8(VN_OP_NARR); c16(1);
    c8(VN_OP_END_THREAD);
    vn_wr32(g_codebuf + spawn_at + 1, second);
    finish_program();

    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    vn_run(&m, &g_prog, &pr, 0, 1000);

    CHECK(m.error == 0, "threaded run trapped: %s", m.error_msg ? m.error_msg : "?");
    CHECK(m.current != 0xFF, "no thread claimed the screen");
    int blocked_say = 0, waiting = 0;
    for (int i = 0; i < VN_MAX_THREADS; i++) {
        if (m.thread[i].state == VN_TH_BLOCKED_SAY) blocked_say++;
        if (m.thread[i].state == VN_TH_BLOCKED_YIELD) waiting++;
    }
    CHECK(blocked_say == 1, "%d threads think they own the screen, want 1", blocked_say);
    CHECK(waiting == 1, "%d threads waiting their turn, want 1", waiting);

    /* Dismissing the line must hand the screen to the other thread. */
    vn_advance(&m);                /* completes the reveal */
    vn_advance(&m);                /* dismisses */
    CHECK(m.current == 0xFF, "screen not released after advance");
    vn_run(&m, &g_prog, &pr, 0, 1000);
    CHECK(m.current != 0xFF, "second thread never got the screen");
}

static void test_choice_guard(void)
{
    reset();
    c8(VN_OP_CHOICE_BEGIN);
    c8(VN_OP_PUSH); c32(1);
    c8(VN_OP_CHOICE_ADD); c16(0); c32(0);
    c8(VN_OP_PUSH); c32(0);                 /* disabled option */
    c8(VN_OP_CHOICE_ADD); c16(1); c32(0);
    c8(VN_OP_CHOICE_SHOW);
    c8(VN_OP_HALT);
    finish_program();

    static vn_machine m; static vn_profile pr;
    vn_machine_init(&m, &g_prog, 0);
    vn_profile_init(&pr);
    vn_run(&m, &g_prog, &pr, 0, 1000);

    CHECK(vn_choice_count(&m) == 2, "expected 2 options, got %d", vn_choice_count(&m));
    CHECK(vn_choice_enabled(&m, 0) == 1, "guarded-true option came out disabled");
    CHECK(vn_choice_enabled(&m, 1) == 0, "guarded-false option came out enabled");
    CHECK(vn_choose(&m, 1) == 0, "a disabled option was selectable");
    CHECK(vn_choose(&m, 9) == 0, "an out-of-range option was selectable");
    CHECK(vn_choose(&m, 0) == 1, "the enabled option was not selectable");
}

static void test_read_registry(void)
{
    static vn_profile pr;
    vn_profile_init(&pr);
    CHECK(vn_is_read(&pr, 40) == 0, "fresh profile reports text as read");
    vn_mark_read(&pr, 40);
    CHECK(vn_is_read(&pr, 40) == 1, "marked text does not read back");
    CHECK(vn_is_read(&pr, 41) == 0, "marking one address marked its neighbour");
}

int main(void)
{
    printf("vm tests\n");
    test_saturating_arithmetic();
    test_comparisons_and_logic();
    test_call_depth_bounded();
    test_expr_stack_bounded();
    test_dirty_stack_is_trapped();
    test_bad_bytecode_is_rejected();
    test_threads_and_screen_ownership();
    test_choice_guard();
    test_read_registry();

    if (g_fail == 0) { printf("  all passed\n"); return 0; }
    printf("  %d failure(s)\n", g_fail);
    return 1;
}
