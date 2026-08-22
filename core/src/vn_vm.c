#include "vn_vm.h"

#include <string.h>

/* Error codes surfaced through vn_machine.error. A trapped VM stops
 * cleanly with a message rather than corrupting state - important
 * because the same bytecode runs on a watch where there is no debugger. */
enum {
    VN_ERR_NONE = 0,
    VN_ERR_BAD_OP,
    VN_ERR_PC_RANGE,
    VN_ERR_STACK_OVER,
    VN_ERR_STACK_UNDER,
    VN_ERR_CALL_OVER,
    VN_ERR_CALL_UNDER,
    VN_ERR_BANK,
    VN_ERR_SLOT,
    VN_ERR_CHOICE_OVER,
    VN_ERR_NO_THREAD,
    VN_ERR_DIRTY_STACK
};

#define NO_THREAD 0xFF

static void trap(vn_machine *m, int code, const char *msg)
{
    if (m->error) return;              /* keep the first failure */
    m->error = (uint8_t)code;
    m->error_msg = msg;
    m->halted = 1;
}

/* --- saturating arithmetic --------------------------------------
 * Copied from AliceSoft's ADV language (research part 1.8): addition
 * clamps at the maximum, subtraction at the minimum, and nothing ever
 * wraps. Affection counters and flags are exactly the case where silent
 * wraparound produces an unreproducible bug three routes later. */
static int32_t sat_add(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a + (int64_t)b;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

static int32_t sat_sub(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a - (int64_t)b;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

static int32_t sat_mul(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a * (int64_t)b;
    if (r > INT32_MAX) return INT32_MAX;
    if (r < INT32_MIN) return INT32_MIN;
    return (int32_t)r;
}

/* --- loading ----------------------------------------------------- */

int vn_program_load(vn_program *p, const uint8_t *data, size_t n)
{
    if (n < VN_VNB_HEADER_SIZE) return 0;
    if (memcmp(data, VN_VNB_MAGIC, 4) != 0) return 0;
    if (vn_rd16(data + 4) != VN_VNB_VERSION) return 0;

    const uint8_t *h = data + 8;
    p->script_hash = vn_rd32(h + 0);
    uint32_t code_off  = vn_rd32(h + 4);
    uint32_t code_len  = vn_rd32(h + 8);
    uint32_t label_off = vn_rd32(h + 12);
    uint32_t label_cnt = vn_rd32(h + 16);
    uint32_t sym_off   = vn_rd32(h + 20);
    uint32_t sym_cnt   = vn_rd32(h + 24);
    for (int i = 0; i < VN_BANK_COUNT; i++)
        p->slots[i] = vn_rd32(h + 28 + 4 * i);

    /* Every span must lie inside the blob. A malformed .vnb must fail
     * to load, never read past the end. */
    if ((size_t)code_off + code_len > n) return 0;
    if ((size_t)label_off + (size_t)label_cnt * VN_LABEL_REC_SIZE > n) return 0;
    if ((size_t)sym_off + (size_t)sym_cnt * VN_SYM_REC_SIZE > n) return 0;
    if (p->slots[VN_BANK_F] > VN_F_SLOTS)  return 0;
    if (p->slots[VN_BANK_SF] > VN_SF_SLOTS) return 0;
    if (p->slots[VN_BANK_TF] > VN_TF_SLOTS) return 0;
    if (p->slots[VN_BANK_L] > VN_THREAD_LOCALS) return 0;

    p->blob = data;
    p->blob_len = n;
    p->code = data + code_off;
    p->code_len = code_len;
    p->labels = data + label_off;
    p->label_count = label_cnt;
    p->syms = data + sym_off;
    p->sym_count = sym_cnt;
    return 1;
}

int vn_strings_load(vn_strings *s, const uint8_t *data, size_t n)
{
    if (n < 8) return 0;
    if (memcmp(data, VN_VNSTR_MAGIC, 4) != 0) return 0;

    uint32_t count = vn_rd32(data + 4);
    /* layout: header, count+1 offsets, count source hashes, then data */
    size_t table = 8 + (size_t)(count + 1) * 4 + (size_t)count * 4;
    if (table < 8 || table > n) return 0;

    s->blob = data;
    s->blob_len = n;
    s->count = count;
    s->offsets = data + 8;
    s->hashes = data + 8 + (size_t)(count + 1) * 4;
    s->data = (const char *)(data + table);

    /* Offsets must be monotonic and inside the blob, or vn_str could
     * hand a draw loop a pointer past the end. */
    size_t data_len = n - table;
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= count; i++) {
        uint32_t off = vn_rd32(s->offsets + 4 * i);
        if (off < prev || off > data_len) return 0;
        prev = off;
    }
    return 1;
}

const char *vn_str(const vn_strings *s, uint16_t id)
{
    if (!s || id == VN_STR_NONE || id >= s->count) return "";
    return s->data + vn_rd32(s->offsets + 4 * id);
}

uint32_t vn_str_hash(const vn_strings *s, uint16_t id)
{
    if (!s || id == VN_STR_NONE || id >= s->count) return 0;
    return vn_rd32(s->hashes + 4 * id);
}

int vn_str_by_hash(const vn_strings *s, uint32_t hash, uint16_t *id)
{
    if (!s || hash == 0) { *id = VN_STR_NONE; return 0; }
    for (uint32_t i = 0; i < s->count; i++)
        if (vn_rd32(s->hashes + 4 * i) == hash) { *id = (uint16_t)i; return 1; }
    *id = VN_STR_NONE;
    return 0;
}

int vn_find_label(const vn_program *p, uint32_t name_hash,
                  uint32_t *addr, uint32_t *body_hash)
{
    for (uint32_t i = 0; i < p->label_count; i++) {
        const uint8_t *r = p->labels + (size_t)i * VN_LABEL_REC_SIZE;
        if (vn_rd32(r) == name_hash) {
            if (addr) *addr = vn_rd32(r + 4);
            if (body_hash) *body_hash = vn_rd32(r + 8);
            return 1;
        }
    }
    return 0;
}

int vn_find_symbol(const vn_program *p, uint32_t name_hash,
                   uint8_t *bank, uint16_t *slot)
{
    for (uint32_t i = 0; i < p->sym_count; i++) {
        const uint8_t *r = p->syms + (size_t)i * VN_SYM_REC_SIZE;
        if (vn_rd32(r) == name_hash) {
            if (bank) *bank = r[4];
            if (slot) *slot = vn_rd16(r + 6);
            return 1;
        }
    }
    return 0;
}

/* --- init --------------------------------------------------------- */

void vn_machine_init(vn_machine *m, const vn_program *p, uint32_t entry_addr)
{
    (void)p;
    memset(m, 0, sizeof *m);
    m->current = NO_THREAD;
    m->rng = 0x9E3779B9u;
    m->stage.say_speaker = VN_STR_NONE;
    m->stage.say_text = VN_STR_NONE;
    m->stage.bg = VN_STR_NONE;
    m->stage.bgm = VN_STR_NONE;
    m->stage.sfx_pending = VN_STR_NONE;
    for (int i = 0; i < VN_CHAR_SLOTS; i++) m->stage.chara[i].asset = VN_STR_NONE;
    m->stage.reveal = -1;

    for (int i = 0; i < VN_MAX_THREADS; i++)
        for (int j = 0; j < VN_MAX_CHOICES; j++)
            m->thread[i].choice_text[j] = VN_STR_NONE;

    m->thread[0].state = VN_TH_RUN;
    m->thread[0].pc = entry_addr;
}

void vn_profile_init(vn_profile *pr) { memset(pr, 0, sizeof *pr); }

/* --- read-text registry -------------------------------------------
 * One bit per code address, per RealLive's dynamic_bitset (research
 * part 2.2). KAG's alternative - one named system variable per label -
 * produces tens of thousands of entries at FSN scale. */
void vn_mark_read(vn_profile *pr, uint32_t addr)
{
    uint32_t bit = addr % (uint32_t)(sizeof pr->read_bits * 8);
    pr->read_bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

int vn_is_read(const vn_profile *pr, uint32_t addr)
{
    uint32_t bit = addr % (uint32_t)(sizeof pr->read_bits * 8);
    return (pr->read_bits[bit >> 3] >> (bit & 7)) & 1;
}

/* --- variable access ---------------------------------------------- */

static int32_t load_var(vn_machine *m, vn_profile *pr, vn_thread *th,
                        uint8_t bank, uint16_t slot)
{
    switch (bank) {
    case VN_BANK_F:  if (slot >= VN_F_SLOTS) break; return m->f[slot];
    case VN_BANK_SF: if (slot >= VN_SF_SLOTS) break; return pr->sf[slot];
    case VN_BANK_TF: if (slot >= VN_TF_SLOTS) break; return m->tf[slot];
    case VN_BANK_L:  if (slot >= VN_THREAD_LOCALS) break; return th->local[slot];
    default: trap(m, VN_ERR_BANK, "load from unknown bank"); return 0;
    }
    trap(m, VN_ERR_SLOT, "load out of bank range");
    return 0;
}

static void store_var(vn_machine *m, vn_profile *pr, vn_thread *th,
                      uint8_t bank, uint16_t slot, int32_t v)
{
    switch (bank) {
    case VN_BANK_F:  if (slot >= VN_F_SLOTS) break; m->f[slot] = v; return;
    case VN_BANK_SF: if (slot >= VN_SF_SLOTS) break; pr->sf[slot] = v; return;
    case VN_BANK_TF: if (slot >= VN_TF_SLOTS) break; m->tf[slot] = v; return;
    case VN_BANK_L:  if (slot >= VN_THREAD_LOCALS) break; th->local[slot] = v; return;
    default: trap(m, VN_ERR_BANK, "store to unknown bank"); return;
    }
    trap(m, VN_ERR_SLOT, "store out of bank range");
}

/* --- expression stack ---------------------------------------------
 * Bounded and checked. The compiler guarantees the stack is empty at
 * every blocking instruction; the VM asserts it (see check_clean).
 * Together those two facts are what make a snapshot a flat memcpy. */
static void push(vn_machine *m, vn_thread *th, int32_t v)
{
    if (th->sp >= VN_EXPR_STACK) { trap(m, VN_ERR_STACK_OVER, "expression stack overflow"); return; }
    th->stack[th->sp++] = v;
}

static int32_t pop(vn_machine *m, vn_thread *th)
{
    if (th->sp == 0) { trap(m, VN_ERR_STACK_UNDER, "expression stack underflow"); return 0; }
    int32_t v = th->stack[--th->sp];
    /* Clear the vacated slot. Dead storage is always zero, so the
     * machine has exactly one representation for a given logical state
     * - which is what makes a snapshot comparison meaningful and keeps
     * stale text ids out of save files. */
    th->stack[th->sp] = 0;
    return v;
}

static void clear_choices(vn_thread *th)
{
    th->choice_count = 0;
    for (int i = 0; i < VN_MAX_CHOICES; i++) {
        th->choice_text[i] = VN_STR_NONE;
        th->choice_target[i] = 0;
        th->choice_enabled[i] = 0;
    }
}

static void kill_thread(vn_thread *th)
{
    memset(th, 0, sizeof *th);
    th->state = VN_TH_DEAD;
    for (int i = 0; i < VN_MAX_CHOICES; i++) th->choice_text[i] = VN_STR_NONE;
}

static void check_clean(vn_machine *m, vn_thread *th, const char *what)
{
    if (th->sp != 0) trap(m, VN_ERR_DIRTY_STACK, what);
}

static int spawn_thread(vn_machine *m, uint32_t addr)
{
    for (int i = 0; i < VN_MAX_THREADS; i++) {
        if (m->thread[i].state == VN_TH_DEAD) {
            vn_thread *t = &m->thread[i];
            kill_thread(t);
            t->state = VN_TH_RUN;
            t->pc = addr;
            return i;
        }
    }
    trap(m, VN_ERR_NO_THREAD, "no free thread slot for spawn");
    return -1;
}

/* Execute one instruction of `th`. Returns 1 if the thread may keep
 * running this pass, 0 if it blocked or died. */
static int step(vn_machine *m, const vn_program *p, vn_profile *pr,
                vn_thread *th, uint32_t now_ms)
{
    if (th->pc >= p->code_len) { trap(m, VN_ERR_PC_RANGE, "pc past end of code"); return 0; }

    uint8_t op = p->code[th->pc];
    int len = vn_op_len(op);
    if (len < 0) { trap(m, VN_ERR_BAD_OP, "unknown opcode"); return 0; }
    if (th->pc + (uint32_t)len > p->code_len) {
        trap(m, VN_ERR_PC_RANGE, "instruction truncated at end of code");
        return 0;
    }

    const uint8_t *a = p->code + th->pc + 1;
    uint32_t next = th->pc + (uint32_t)len;
    m->steps++;

    switch (op) {
    case VN_OP_HALT:
        m->halted = 1;
        kill_thread(th);
        return 0;

    case VN_OP_NOP: break;

    case VN_OP_PUSH:  push(m, th, (int32_t)vn_rd32(a)); break;
    case VN_OP_LOAD:  push(m, th, load_var(m, pr, th, a[0], vn_rd16(a + 1))); break;
    case VN_OP_STORE: store_var(m, pr, th, a[0], vn_rd16(a + 1), pop(m, th)); break;

    case VN_OP_ADD: { int32_t b = pop(m, th); push(m, th, sat_add(pop(m, th), b)); break; }
    case VN_OP_SUB: { int32_t b = pop(m, th); push(m, th, sat_sub(pop(m, th), b)); break; }
    case VN_OP_MUL: { int32_t b = pop(m, th); push(m, th, sat_mul(pop(m, th), b)); break; }

    case VN_OP_EQ: { int32_t b = pop(m, th); push(m, th, pop(m, th) == b); break; }
    case VN_OP_NE: { int32_t b = pop(m, th); push(m, th, pop(m, th) != b); break; }
    case VN_OP_LT: { int32_t b = pop(m, th); push(m, th, pop(m, th) <  b); break; }
    case VN_OP_LE: { int32_t b = pop(m, th); push(m, th, pop(m, th) <= b); break; }
    case VN_OP_GT: { int32_t b = pop(m, th); push(m, th, pop(m, th) >  b); break; }
    case VN_OP_GE: { int32_t b = pop(m, th); push(m, th, pop(m, th) >= b); break; }

    case VN_OP_AND: { int32_t b = pop(m, th); int32_t x = pop(m, th); push(m, th, (x != 0) && (b != 0)); break; }
    case VN_OP_OR:  { int32_t b = pop(m, th); int32_t x = pop(m, th); push(m, th, (x != 0) || (b != 0)); break; }
    case VN_OP_NOT: push(m, th, pop(m, th) == 0); break;

    case VN_OP_JMP: th->pc = vn_rd32(a); return 1;
    case VN_OP_JZ:
        if (pop(m, th) == 0) { th->pc = vn_rd32(a); return 1; }
        break;

    case VN_OP_CALL:
        if (th->call_depth >= VN_CALL_DEPTH) {
            trap(m, VN_ERR_CALL_OVER, "call stack overflow");
            return 0;
        }
        th->call_stack[th->call_depth++] = next;
        th->pc = vn_rd32(a);
        return 1;

    case VN_OP_RET:
        if (th->call_depth == 0) { trap(m, VN_ERR_CALL_UNDER, "return with empty call stack"); return 0; }
        th->pc = th->call_stack[--th->call_depth];
        th->call_stack[th->call_depth] = 0;
        return 1;

    case VN_OP_SPAWN:
        spawn_thread(m, vn_rd32(a));
        break;

    case VN_OP_YIELD:
        check_clean(m, th, "yield with a non-empty expression stack");
        th->pc = next;
        th->state = VN_TH_BLOCKED_YIELD;
        return 0;

    case VN_OP_END_THREAD:
        kill_thread(th);
        return 0;

    case VN_OP_SAY:
    case VN_OP_NARR: {
        check_clean(m, th, "say with a non-empty expression stack");
        /* Only one thread may own the screen. A second thread reaching
         * a line while another is still displaying simply retries next
         * frame - it must NOT consume the instruction. */
        int me = (int)(th - m->thread);
        if (m->current != NO_THREAD && m->current != (uint8_t)me) {
            th->state = VN_TH_BLOCKED_YIELD;
            return 0;                    /* pc deliberately unchanged */
        }
        if (op == VN_OP_SAY) {
            m->stage.say_speaker = vn_rd16(a);
            m->stage.say_text = vn_rd16(a + 2);
        } else {
            m->stage.say_speaker = VN_STR_NONE;
            m->stage.say_text = vn_rd16(a);
        }
        m->stage.reveal = 0;
        vn_mark_read(pr, th->pc);
        m->current = (uint8_t)me;
        th->pc = next;
        th->state = VN_TH_BLOCKED_SAY;
        return 0;
    }

    case VN_OP_BG:
        m->stage.bg = vn_rd16(a);
        m->stage.bg_fade_ms = vn_rd16(a + 2);
        break;

    case VN_OP_SHOW: {
        uint8_t slot = a[2];
        if (slot >= VN_CHAR_SLOTS) { trap(m, VN_ERR_SLOT, "character slot out of range"); return 0; }
        m->stage.chara[slot].asset = vn_rd16(a);
        m->stage.chara[slot].pos = a[3];
        m->stage.chara[slot].visible = 1;
        break;
    }

    case VN_OP_HIDE: {
        uint8_t slot = a[0];
        if (slot >= VN_CHAR_SLOTS) { trap(m, VN_ERR_SLOT, "character slot out of range"); return 0; }
        m->stage.chara[slot].visible = 0;
        break;
    }

    case VN_OP_BGM: m->stage.bgm = vn_rd16(a); break;
    case VN_OP_SFX: m->stage.sfx_pending = vn_rd16(a); break;

    case VN_OP_WAIT:
        check_clean(m, th, "wait with a non-empty expression stack");
        th->wait_until = now_ms + vn_rd16(a);
        th->pc = next;
        th->state = VN_TH_BLOCKED_WAIT;
        return 0;

    case VN_OP_CHOICE_BEGIN:
        clear_choices(th);
        break;

    case VN_OP_CHOICE_ADD: {
        int32_t enabled = pop(m, th);
        if (th->choice_count >= VN_MAX_CHOICES) {
            trap(m, VN_ERR_CHOICE_OVER, "too many choice options");
            return 0;
        }
        uint8_t i = th->choice_count++;
        th->choice_text[i] = vn_rd16(a);
        th->choice_target[i] = vn_rd32(a + 2);
        th->choice_enabled[i] = enabled != 0;
        break;
    }

    case VN_OP_CHOICE_SHOW: {
        check_clean(m, th, "choice with a non-empty expression stack");
        int me = (int)(th - m->thread);
        if (m->current != NO_THREAD && m->current != (uint8_t)me) {
            th->state = VN_TH_BLOCKED_YIELD;
            return 0;
        }
        m->current = (uint8_t)me;
        th->pc = next;
        th->state = VN_TH_BLOCKED_CHOICE;
        return 0;
    }

    case VN_OP_ENDING: {
        uint16_t id = vn_rd16(a);
        m->stage.ending_id = id;
        m->stage.ending_kind = a[2];
        m->stage.ending_active = 1;
        /* An ending is a tracked content node, not a failure state
         * (research part 2.3). Recording it is the whole point. */
        if ((id >> 3) < sizeof pr->ending_seen)
            pr->ending_seen[id >> 3] |= (uint8_t)(1u << (id & 7));
        break;
    }

    default:
        trap(m, VN_ERR_BAD_OP, "opcode not implemented");
        return 0;
    }

    if (m->error) return 0;
    th->pc = next;
    return 1;
}

vn_run_result vn_run(vn_machine *m, const vn_program *p, vn_profile *pr,
                     uint32_t now_ms, uint32_t budget)
{
    if (m->error) return VN_RUN_ERROR;
    if (m->halted) return VN_RUN_HALTED;

    /* Wake threads whose block condition has cleared. A yield resumes
     * on the NEXT call, not this one, so a script that yields in a loop
     * cannot spin the scheduler forever. */
    for (int i = 0; i < VN_MAX_THREADS; i++) {
        vn_thread *t = &m->thread[i];
        if (t->state == VN_TH_BLOCKED_YIELD) t->state = VN_TH_RUN;
        else if (t->state == VN_TH_BLOCKED_WAIT &&
                 (int32_t)(now_ms - t->wait_until) >= 0) t->state = VN_TH_RUN;
    }

    uint32_t used = 0;
    int progressed = 1;
    while (progressed && used < budget && !m->halted && !m->error) {
        progressed = 0;
        for (int i = 0; i < VN_MAX_THREADS && used < budget; i++) {
            vn_thread *t = &m->thread[i];
            while (t->state == VN_TH_RUN && used < budget) {
                if (!step(m, p, pr, t, now_ms)) break;
                used++;
                progressed = 1;
            }
            if (m->halted || m->error) break;
        }
    }

    if (m->error) return VN_RUN_ERROR;
    if (m->halted) return VN_RUN_HALTED;

    /* With every thread dead there is nothing left to wake, so report
     * halted rather than blocking forever. */
    for (int i = 0; i < VN_MAX_THREADS; i++)
        if (m->thread[i].state != VN_TH_DEAD) return VN_RUN_BLOCKED;
    m->halted = 1;
    return VN_RUN_HALTED;
}

/* --- host interaction ---------------------------------------------- */

void vn_advance(vn_machine *m)
{
    if (m->current == NO_THREAD) return;
    vn_thread *t = &m->thread[m->current];
    if (t->state != VN_TH_BLOCKED_SAY) return;

    /* If the line is still typing out, the first advance completes it;
     * the second dismisses it. This is universal VN behaviour and it
     * belongs in the engine, not in every script. */
    if (m->stage.reveal >= 0) { m->stage.reveal = -1; return; }

    t->state = VN_TH_RUN;
    m->current = NO_THREAD;
    m->stage.say_speaker = VN_STR_NONE;
    m->stage.say_text = VN_STR_NONE;
}

int vn_choice_count(const vn_machine *m)
{
    if (m->current == NO_THREAD) return 0;
    const vn_thread *t = &m->thread[m->current];
    return t->state == VN_TH_BLOCKED_CHOICE ? t->choice_count : 0;
}

uint16_t vn_choice_text(const vn_machine *m, int i)
{
    if (i < 0 || i >= vn_choice_count(m)) return VN_STR_NONE;
    return m->thread[m->current].choice_text[i];
}

int vn_choice_enabled(const vn_machine *m, int i)
{
    if (i < 0 || i >= vn_choice_count(m)) return 0;
    return m->thread[m->current].choice_enabled[i];
}

int vn_choose(vn_machine *m, int index)
{
    if (index < 0 || index >= vn_choice_count(m)) return 0;
    vn_thread *t = &m->thread[m->current];
    if (!t->choice_enabled[index]) return 0;

    t->pc = t->choice_target[index];
    clear_choices(t);
    t->state = VN_TH_RUN;
    m->current = NO_THREAD;
    return 1;
}
