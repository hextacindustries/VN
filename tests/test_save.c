/* Save/restore tests.
 *
 * The first test here is the proof obligation for the whole
 * architecture. The design argues (docs/research.md part 2.6) that a
 * VM with no registers, no locals and no closures can be snapshotted by
 * a flat copy, making save-anywhere free. The way to know that is true
 * is to snapshot at an arbitrary point, run on, restore, run the same
 * distance again, and demand the two runs be BIT-IDENTICAL.
 */
#include "vn_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) {                                               \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);            \
        printf(__VA_ARGS__); printf("\n");                       \
        g_fail++;                                                \
    }                                                            \
} while (0)

/* --- loading helpers ------------------------------------------------ */

typedef struct {
    uint8_t   *code_blob, *str_blob;
    vn_program prog;
    vn_strings strs;
} module;

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "short read %s\n", path); exit(2); }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void load_module(module *m, const char *vnb, const char *vnstr)
{
    size_t cn, sn;
    m->code_blob = slurp(vnb, &cn);
    m->str_blob = slurp(vnstr, &sn);
    if (!vn_program_load(&m->prog, m->code_blob, cn)) { fprintf(stderr, "bad vnb %s\n", vnb); exit(2); }
    if (!vn_strings_load(&m->strs, m->str_blob, sn)) { fprintf(stderr, "bad vnstr %s\n", vnstr); exit(2); }
}

/* --- a deterministic driver ------------------------------------------
 * Plays the script without a human: advances lines, picks choices from
 * a seeded PRNG, and moves a virtual clock when everything is waiting.
 * Determinism is the point - two runs from the same state with the same
 * seed must be indistinguishable. */
typedef struct { uint32_t rng, clock; } driver;

static uint32_t nextrand(driver *d)
{
    d->rng = d->rng * 1664525u + 1013904223u;
    return d->rng >> 8;
}

static int beat(vn_machine *m, const vn_program *p, vn_profile *pr, driver *d)
{
    if (vn_run(m, p, pr, d->clock, 100000) != VN_RUN_BLOCKED) return 0;

    int n = vn_choice_count(m);
    if (n > 0) {
        int pick = -1;
        for (int t = 0; t < 32 && pick < 0; t++) {
            int i = (int)(nextrand(d) % (uint32_t)n);
            if (vn_choice_enabled(m, i)) pick = i;
        }
        for (int i = 0; i < n && pick < 0; i++) if (vn_choice_enabled(m, i)) pick = i;
        if (pick < 0) return 0;
        return vn_choose(m, pick);
    }

    if (m->current != 0xFF && m->thread[m->current].state == VN_TH_BLOCKED_SAY) {
        vn_advance(m);   /* finish the typewriter reveal */
        vn_advance(m);   /* dismiss the line */
        return 1;
    }

    d->clock += 100;     /* nothing to do but let time pass */
    return 1;
}

/* The tf bank is transient by definition: scratch that is deliberately
 * NOT written to saves. Comparing it would be comparing a value the
 * design promises not to preserve, so blank it on both sides. Its
 * semantics get their own test below. */
static int machines_agree(const vn_machine *a, const vn_machine *b)
{
    static vn_machine x, y;
    x = *a; y = *b;
    memset(x.tf, 0, sizeof x.tf);
    memset(y.tf, 0, sizeof y.tf);
    return memcmp(&x, &y, sizeof x) == 0;
}

/* --- the proof obligation -------------------------------------------- */

static uint8_t g_save[1 << 16];
static uint8_t g_prof_save[1 << 16];

static void test_roundtrip_is_bit_identical(module *mod)
{
    size_t bound = vn_save_bound();
    CHECK(bound <= sizeof g_save, "save bound %zu exceeds the test buffer", bound);

    int checked = 0;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        for (int prefix = 0; prefix < 40; prefix++) {
            static vn_machine m, restored;
            static vn_profile pr, pr_restored;
            driver d = { seed, 0 };

            vn_machine_init(&m, &mod->prog, 0);
            vn_profile_init(&pr);

            /* Run an arbitrary distance in. */
            int alive = 1;
            for (int i = 0; i < prefix && alive; i++) alive = beat(&m, &mod->prog, &pr, &d);
            if (!alive) break;

            /* Snapshot machine, profile and the driver's own state. */
            size_t sn = 0, pn = 0;
            vn_save_status st = vn_save_write(&m, &mod->prog, &mod->strs,
                                              g_save, sizeof g_save, &sn);
            CHECK(st == VN_SAVE_OK, "save failed at prefix %d: %s", prefix, vn_save_status_str(st));
            st = vn_profile_write(&pr, &mod->prog, g_prof_save, sizeof g_prof_save, &pn);
            CHECK(st == VN_SAVE_OK, "profile save failed: %s", vn_save_status_str(st));
            driver saved_driver = d;

            /* Continue for a while and record where we end up. */
            for (int i = 0; i < 25; i++) if (!beat(&m, &mod->prog, &pr, &d)) break;

            /* Restore and replay exactly the same distance. */
            st = vn_save_read(&restored, &mod->prog, &mod->strs, g_save, sn);
            CHECK(st == VN_SAVE_OK, "load failed at prefix %d: %s", prefix, vn_save_status_str(st));
            st = vn_profile_read(&pr_restored, &mod->prog, g_prof_save, pn);
            CHECK(st == VN_SAVE_OK, "profile load failed: %s", vn_save_status_str(st));
            driver d2 = saved_driver;
            for (int i = 0; i < 25; i++) if (!beat(&restored, &mod->prog, &pr_restored, &d2)) break;

            if (!machines_agree(&m, &restored)) {
                CHECK(0, "seed %u prefix %d: replay after restore diverged "
                         "(steps %llu vs %llu, pc %u vs %u)",
                      seed, prefix,
                      (unsigned long long)m.steps, (unsigned long long)restored.steps,
                      m.thread[0].pc, restored.thread[0].pc);
                return;
            }
            CHECK(memcmp(&pr, &pr_restored, sizeof pr) == 0,
                  "seed %u prefix %d: profile diverged after restore", seed, prefix);
            checked++;
        }
    }
    printf("  round-trip verified at %d distinct save points\n", checked);
}

/* Snapshot specifically at each kind of blocking instruction. */
static void test_save_at_every_block_kind(module *mod)
{
    static vn_machine m; static vn_profile pr;
    driver d = { 7, 0 };
    vn_machine_init(&m, &mod->prog, 0);
    vn_profile_init(&pr);

    int seen_say = 0, seen_choice = 0, seen_wait = 0;
    for (int i = 0; i < 400; i++) {
        if (vn_run(&m, &mod->prog, &pr, d.clock, 100000) != VN_RUN_BLOCKED) break;

        int kind = 0;
        if (vn_choice_count(&m) > 0) kind = 2;
        else if (m.current != 0xFF && m.thread[m.current].state == VN_TH_BLOCKED_SAY) kind = 1;
        else {
            for (int t = 0; t < VN_MAX_THREADS; t++)
                if (m.thread[t].state == VN_TH_BLOCKED_WAIT) { kind = 3; break; }
        }

        if (kind) {
            size_t sn = 0;
            vn_save_status st = vn_save_write(&m, &mod->prog, &mod->strs, g_save, sizeof g_save, &sn);
            CHECK(st == VN_SAVE_OK, "save at block kind %d failed: %s", kind, vn_save_status_str(st));
            static vn_machine back;
            st = vn_save_read(&back, &mod->prog, &mod->strs, g_save, sn);
            CHECK(st == VN_SAVE_OK, "load at block kind %d failed: %s", kind, vn_save_status_str(st));
            CHECK(machines_agree(&m, &back), "restore at block kind %d differs", kind);
            if (kind == 1) seen_say = 1;
            if (kind == 2) seen_choice = 1;
            if (kind == 3) seen_wait = 1;
        }
        if (!beat(&m, &mod->prog, &pr, &d)) break;
    }
    CHECK(seen_say, "never reached a dialogue block");
    CHECK(seen_choice, "never reached a choice block");
    CHECK(seen_wait, "never reached a timed wait block");
}

/* --- script-edit compatibility ---------------------------------------- */

static size_t save_inside_middle(module *mod, vn_machine *m, vn_profile *pr)
{
    driver d = { 3, 0 };
    vn_machine_init(m, &mod->prog, 0);
    vn_profile_init(pr);
    /* Two beats puts us inside `middle`, past its first line. */
    beat(m, &mod->prog, pr, &d);
    beat(m, &mod->prog, pr, &d);
    size_t sn = 0;
    vn_save_status st = vn_save_write(m, &mod->prog, &mod->strs, g_save, sizeof g_save, &sn);
    CHECK(st == VN_SAVE_OK, "baseline save failed: %s", vn_save_status_str(st));
    return sn;
}

static void test_edit_elsewhere_keeps_saves(module *base, module *other, module *same, module *addvar)
{
    static vn_machine m; static vn_profile pr;
    size_t sn = save_inside_middle(base, &m, &pr);
    int32_t a_before = m.f[0];

    /* A scene BEFORE the save point grew, shifting every later address.
     * Label-relative addressing must absorb that. */
    static vn_machine loaded;
    vn_save_status st = vn_save_read(&loaded, &other->prog, &other->strs, g_save, sn);
    CHECK(st == VN_SAVE_OK,
          "editing a different scene broke the save: %s", vn_save_status_str(st));
    CHECK(loaded.f[0] == a_before, "variable value lost across an unrelated edit");

    /* The scene the save sits in changed: restoring would resume at a
     * meaningless offset, so it must be refused rather than corrupt. */
    st = vn_save_read(&loaded, &same->prog, &same->strs, g_save, sn);
    CHECK(st == VN_SAVE_E_LABEL_CHANGED,
          "editing the saved scene should be refused, got: %s", vn_save_status_str(st));

    /* A new variable was introduced ahead of f.a, shifting its slot.
     * Name-hash remapping must still land the value in the right place. */
    uint8_t bank; uint16_t slot_base = 0, slot_new = 0;
    CHECK(vn_find_symbol(&base->prog, vn_hash("f.a"), &bank, &slot_base), "f.a missing in base");
    CHECK(vn_find_symbol(&addvar->prog, vn_hash("f.a"), &bank, &slot_new), "f.a missing in add_var");
    CHECK(slot_base != slot_new, "add_var did not actually shift f.a's slot (test is not testing anything)");

    st = vn_save_read(&loaded, &addvar->prog, &addvar->strs, g_save, sn);
    CHECK(st == VN_SAVE_OK, "adding a variable broke the save: %s", vn_save_status_str(st));
    CHECK(loaded.f[slot_new] == a_before,
          "f.a came back as %d, want %d, after its slot moved from %u to %u",
          loaded.f[slot_new], a_before, slot_base, slot_new);
}

static void test_corrupt_saves_are_rejected(module *mod)
{
    static vn_machine m; static vn_profile pr;
    size_t sn = save_inside_middle(mod, &m, &pr);

    static vn_machine out;
    uint8_t bad[1 << 16];

    memcpy(bad, g_save, sn);
    bad[0] = 'X';
    CHECK(vn_save_read(&out, &mod->prog, &mod->strs, bad, sn) == VN_SAVE_E_MAGIC,
          "bad magic accepted");

    memcpy(bad, g_save, sn);
    vn_wr16(bad + 4, 999);
    CHECK(vn_save_read(&out, &mod->prog, &mod->strs, bad, sn) == VN_SAVE_E_VERSION,
          "bad version accepted");

    for (size_t cut = 1; cut < sn; cut += 7) {
        vn_save_status st = vn_save_read(&out, &mod->prog, &mod->strs, g_save, cut);
        CHECK(st != VN_SAVE_OK, "truncated save of %zu/%zu bytes accepted", cut, sn);
    }
}

/* Document the tf contract explicitly rather than leaving it implied by
 * an exclusion in the comparison above. */
static void test_transient_bank_is_not_saved(module *mod)
{
    static vn_machine m, back; static vn_profile pr;
    driver d = { 11, 0 };
    vn_machine_init(&m, &mod->prog, 0);
    vn_profile_init(&pr);

    int touched = 0;
    for (int i = 0; i < 200 && beat(&m, &mod->prog, &pr, &d); i++) {
        for (int j = 0; j < VN_TF_SLOTS; j++) if (m.tf[j]) { touched = 1; break; }
        if (touched) break;
    }
    CHECK(touched, "demo script never wrote to the transient bank; test is vacuous");

    size_t sn = 0;
    CHECK(vn_save_write(&m, &mod->prog, &mod->strs, g_save, sizeof g_save, &sn) == VN_SAVE_OK,
          "save failed");
    CHECK(vn_save_read(&back, &mod->prog, &mod->strs, g_save, sn) == VN_SAVE_OK, "load failed");

    for (int j = 0; j < VN_TF_SLOTS; j++)
        CHECK(back.tf[j] == 0, "transient slot %d survived a save/load round trip", j);
    CHECK(machines_agree(&m, &back), "everything except tf should have round-tripped");
}

int main(int argc, char **argv)
{
    if (argc < 11) {
        fprintf(stderr, "usage: test_save demo.vnb demo.vnstr base.vnb base.vnstr "
                        "other.vnb other.vnstr same.vnb same.vnstr addvar.vnb addvar.vnstr\n");
        return 2;
    }
    static module demo, base, other, same, addvar;
    load_module(&demo, argv[1], argv[2]);
    load_module(&base, argv[3], argv[4]);
    load_module(&other, argv[5], argv[6]);
    load_module(&same, argv[7], argv[8]);
    load_module(&addvar, argv[9], argv[10]);

    printf("save tests\n");
    test_roundtrip_is_bit_identical(&demo);
    test_save_at_every_block_kind(&demo);
    test_edit_elsewhere_keeps_saves(&base, &other, &same, &addvar);
    test_corrupt_saves_are_rejected(&base);
    test_transient_bank_is_not_saved(&demo);

    if (g_fail == 0) { printf("  all passed\n"); return 0; }
    printf("  %d failure(s)\n", g_fail);
    return 1;
}
