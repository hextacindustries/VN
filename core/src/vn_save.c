#include "vn_save.h"

#include <string.h>

/* --- bounds-checked cursor -----------------------------------------
 * Every read and write goes through these, so a truncated or hostile
 * save file fails cleanly instead of walking off the buffer. */
typedef struct { uint8_t *p; size_t cap, n; int bad; } wbuf;
typedef struct { const uint8_t *p; size_t n, at; int bad; } rbuf;

static void w8(wbuf *b, uint8_t v)
{
    if (b->n + 1 > b->cap) { b->bad = 1; return; }
    b->p[b->n++] = v;
}
static void w16(wbuf *b, uint16_t v) { w8(b, (uint8_t)(v & 0xFF)); w8(b, (uint8_t)(v >> 8)); }
static void w32(wbuf *b, uint32_t v) { w16(b, (uint16_t)(v & 0xFFFF)); w16(b, (uint16_t)(v >> 16)); }
static void w64(wbuf *b, uint64_t v) { w32(b, (uint32_t)(v & 0xFFFFFFFFu)); w32(b, (uint32_t)(v >> 32)); }

static uint8_t r8(rbuf *b)
{
    if (b->at + 1 > b->n) { b->bad = 1; return 0; }
    return b->p[b->at++];
}
static uint16_t r16(rbuf *b) { uint16_t a = r8(b); return (uint16_t)(a | ((uint16_t)r8(b) << 8)); }
static uint32_t r32(rbuf *b) { uint32_t a = r16(b); return a | ((uint32_t)r16(b) << 16); }
static uint64_t r64(rbuf *b) { uint64_t a = r32(b); return a | ((uint64_t)r32(b) << 32); }

const char *vn_save_status_str(vn_save_status st)
{
    switch (st) {
    case VN_SAVE_OK:              return "ok";
    case VN_SAVE_E_MAGIC:         return "not a save file";
    case VN_SAVE_E_VERSION:       return "save from an incompatible version";
    case VN_SAVE_E_TRUNCATED:     return "save file is truncated";
    case VN_SAVE_E_NO_SPACE:      return "buffer too small for save";
    case VN_SAVE_E_LABEL_MISSING: return "the scene this save was made in no longer exists";
    case VN_SAVE_E_LABEL_CHANGED: return "the scene this save was made in has been edited";
    case VN_SAVE_E_RANGE:         return "save contains an out-of-range value";
    default:                      return "unknown save error";
    }
}

/* --- address <-> (label, offset) ------------------------------------ */

/* Find the label containing `addr`: the one with the greatest address
 * not exceeding it. */
static int addr_to_label(const vn_program *p, uint32_t addr,
                         uint32_t *name_hash, uint32_t *body_hash, uint32_t *offset)
{
    uint32_t best_addr = 0;
    const uint8_t *best = NULL;
    for (uint32_t i = 0; i < p->label_count; i++) {
        const uint8_t *r = p->labels + (size_t)i * VN_LABEL_REC_SIZE;
        uint32_t a = vn_rd32(r + 4);
        if (a <= addr && (!best || a >= best_addr)) { best = r; best_addr = a; }
    }
    if (!best) return 0;
    *name_hash = vn_rd32(best + 0);
    *body_hash = vn_rd32(best + 8);
    *offset = addr - best_addr;
    return 1;
}

static vn_save_status label_to_addr(const vn_program *p, uint32_t name_hash,
                                    uint32_t body_hash, uint32_t offset,
                                    uint32_t *addr)
{
    uint32_t a, bh;
    if (!vn_find_label(p, name_hash, &a, &bh)) return VN_SAVE_E_LABEL_MISSING;
    /* The scene this save sits inside must be byte-identical. Scenes
     * elsewhere in the script may have changed freely. */
    if (bh != body_hash) return VN_SAVE_E_LABEL_CHANGED;
    if (a + offset > p->code_len) return VN_SAVE_E_RANGE;
    *addr = a + offset;
    return VN_SAVE_OK;
}

static void write_addr(wbuf *b, const vn_program *p, uint32_t addr)
{
    uint32_t nh = 0, bh = 0, off = 0;
    if (!addr_to_label(p, addr, &nh, &bh, &off)) { nh = 0; bh = 0; off = addr; }
    w32(b, nh); w32(b, bh); w32(b, off);
}

static vn_save_status read_addr(rbuf *b, const vn_program *p, uint32_t *addr)
{
    uint32_t nh = r32(b), bh = r32(b), off = r32(b);
    if (b->bad) return VN_SAVE_E_TRUNCATED;
    if (nh == 0) {                       /* address outside any label */
        if (off > p->code_len) return VN_SAVE_E_RANGE;
        *addr = off;
        return VN_SAVE_OK;
    }
    return label_to_addr(p, nh, bh, off, addr);
}

/* Stage strings are stored by source hash so ids may be renumbered. */
static void write_str(wbuf *b, const vn_strings *s, uint16_t id)
{
    w32(b, id == VN_STR_NONE ? 0u : vn_str_hash(s, id));
}

static uint16_t read_str(rbuf *b, const vn_strings *s)
{
    uint32_t h = r32(b);
    uint16_t id = VN_STR_NONE;
    if (h) vn_str_by_hash(s, h, &id);
    return id;
}

size_t vn_save_bound(void)
{
    /* Generous: every field at its widest, plus header slack. */
    return 512
         + (size_t)VN_MAX_THREADS * (64 + VN_CALL_DEPTH * 12 +
                                     VN_THREAD_LOCALS * 4 + VN_EXPR_STACK * 4 +
                                     VN_MAX_CHOICES * 20)
         + (size_t)VN_F_SLOTS * 8;
}

size_t vn_profile_bound(void)
{
    return 256 + (size_t)VN_SF_SLOTS * 8 + sizeof(((vn_profile *)0)->ending_seen)
         + sizeof(((vn_profile *)0)->read_bits);
}

vn_save_status vn_save_write(const vn_machine *m, const vn_program *p,
                             const vn_strings *s,
                             uint8_t *out, size_t cap, size_t *written)
{
    wbuf b = { out, cap, 0, 0 };

    for (int i = 0; i < 4; i++) w8(&b, (uint8_t)VN_SAVE_MAGIC[i]);
    w16(&b, VN_SAVE_VERSION);
    w16(&b, 0);
    w32(&b, p->script_hash);

    /* --- threads --- */
    w16(&b, VN_MAX_THREADS);
    for (int i = 0; i < VN_MAX_THREADS; i++) {
        const vn_thread *t = &m->thread[i];
        w8(&b, t->state);
        /* A dead thread has no position worth recording. Writing one
         * would make the save depend on whichever label happens to sit
         * at address zero, so an edit there would wrongly reject it. */
        if (t->state == VN_TH_DEAD) continue;
        w8(&b, t->call_depth);
        w8(&b, t->sp);
        w8(&b, t->choice_count);
        w32(&b, t->wait_until);
        write_addr(&b, p, t->pc);
        for (int j = 0; j < t->call_depth; j++) write_addr(&b, p, t->call_stack[j]);
        for (int j = 0; j < VN_THREAD_LOCALS; j++) w32(&b, (uint32_t)t->local[j]);
        for (int j = 0; j < t->sp; j++) w32(&b, (uint32_t)t->stack[j]);
        for (int j = 0; j < t->choice_count; j++) {
            write_str(&b, s, t->choice_text[j]);
            write_addr(&b, p, t->choice_target[j]);
            w8(&b, t->choice_enabled[j]);
        }
    }

    /* --- save-local variables, by NAME HASH ---
     * Only slots the compiler actually allocated are written, and each
     * carries its name, so inserting a flag cannot shift the rest. */
    uint32_t nf = 0;
    for (uint32_t i = 0; i < p->sym_count; i++)
        if (p->syms[(size_t)i * VN_SYM_REC_SIZE + 4] == VN_BANK_F) nf++;
    w32(&b, nf);
    for (uint32_t i = 0; i < p->sym_count; i++) {
        const uint8_t *r = p->syms + (size_t)i * VN_SYM_REC_SIZE;
        if (r[4] != VN_BANK_F) continue;
        uint16_t slot = vn_rd16(r + 6);
        w32(&b, vn_rd32(r));
        w32(&b, slot < VN_F_SLOTS ? (uint32_t)m->f[slot] : 0u);
    }

    /* --- stage --- */
    write_str(&b, s, m->stage.bg);
    w16(&b, m->stage.bg_fade_ms);
    write_str(&b, s, m->stage.bgm);
    for (int i = 0; i < VN_CHAR_SLOTS; i++) {
        write_str(&b, s, m->stage.chara[i].asset);
        w8(&b, m->stage.chara[i].pos);
        w8(&b, m->stage.chara[i].visible);
    }
    write_str(&b, s, m->stage.say_speaker);
    write_str(&b, s, m->stage.say_text);
    w32(&b, (uint32_t)m->stage.reveal);
    w16(&b, m->stage.ending_id);
    w8(&b, m->stage.ending_kind);
    w8(&b, m->stage.ending_active);

    w32(&b, m->rng);
    w64(&b, m->steps);
    w8(&b, m->current);
    w8(&b, m->halted);

    if (b.bad) return VN_SAVE_E_NO_SPACE;
    if (written) *written = b.n;
    return VN_SAVE_OK;
}

vn_save_status vn_save_read(vn_machine *m, const vn_program *p,
                            const vn_strings *s,
                            const uint8_t *in, size_t n)
{
    rbuf b = { in, n, 0, 0 };
    vn_save_status st;

    if (n < 12) return VN_SAVE_E_TRUNCATED;
    if (memcmp(in, VN_SAVE_MAGIC, 4) != 0) return VN_SAVE_E_MAGIC;
    b.at = 4;
    if (r16(&b) != VN_SAVE_VERSION) return VN_SAVE_E_VERSION;
    (void)r16(&b);
    (void)r32(&b);                        /* script hash: advisory only;
                                             per-label hashes decide */

    /* Build into a scratch machine so a failed load cannot leave the
     * live machine half-overwritten. */
    static vn_machine tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.stage.bg = tmp.stage.bgm = tmp.stage.sfx_pending = VN_STR_NONE;
    tmp.stage.say_speaker = tmp.stage.say_text = VN_STR_NONE;
    /* Match the VM's canonical form: unused choice slots hold
     * VN_STR_NONE, not zero. */
    for (int i = 0; i < VN_MAX_THREADS; i++)
        for (int j = 0; j < VN_MAX_CHOICES; j++)
            tmp.thread[i].choice_text[j] = VN_STR_NONE;

    uint16_t nthreads = r16(&b);
    if (nthreads > VN_MAX_THREADS) return VN_SAVE_E_RANGE;
    for (int i = 0; i < nthreads; i++) {
        vn_thread *t = &tmp.thread[i];
        t->state = r8(&b);
        if (b.bad) return VN_SAVE_E_TRUNCATED;
        if (t->state == VN_TH_DEAD) continue;   /* already canonical */
        t->call_depth = r8(&b);
        t->sp = r8(&b);
        t->choice_count = r8(&b);
        t->wait_until = r32(&b);
        if (b.bad) return VN_SAVE_E_TRUNCATED;
        if (t->call_depth > VN_CALL_DEPTH || t->sp > VN_EXPR_STACK ||
            t->choice_count > VN_MAX_CHOICES || t->state > VN_TH_BLOCKED_YIELD)
            return VN_SAVE_E_RANGE;

        if ((st = read_addr(&b, p, &t->pc)) != VN_SAVE_OK) return st;
        for (int j = 0; j < t->call_depth; j++)
            if ((st = read_addr(&b, p, &t->call_stack[j])) != VN_SAVE_OK) return st;
        for (int j = 0; j < VN_THREAD_LOCALS; j++) t->local[j] = (int32_t)r32(&b);
        for (int j = 0; j < t->sp; j++) t->stack[j] = (int32_t)r32(&b);
        for (int j = 0; j < t->choice_count; j++) {
            t->choice_text[j] = read_str(&b, s);
            if ((st = read_addr(&b, p, &t->choice_target[j])) != VN_SAVE_OK) return st;
            t->choice_enabled[j] = r8(&b);
        }
        if (b.bad) return VN_SAVE_E_TRUNCATED;
    }

    uint32_t nf = r32(&b);
    if (b.bad) return VN_SAVE_E_TRUNCATED;
    for (uint32_t i = 0; i < nf; i++) {
        uint32_t name = r32(&b);
        uint32_t val = r32(&b);
        if (b.bad) return VN_SAVE_E_TRUNCATED;
        uint8_t bank; uint16_t slot;
        /* A variable removed from the script is simply dropped; one
         * added since the save keeps its initial value. */
        if (vn_find_symbol(p, name, &bank, &slot) &&
            bank == VN_BANK_F && slot < VN_F_SLOTS)
            tmp.f[slot] = (int32_t)val;
    }

    tmp.stage.bg = read_str(&b, s);
    tmp.stage.bg_fade_ms = r16(&b);
    tmp.stage.bgm = read_str(&b, s);
    for (int i = 0; i < VN_CHAR_SLOTS; i++) {
        tmp.stage.chara[i].asset = read_str(&b, s);
        tmp.stage.chara[i].pos = r8(&b);
        tmp.stage.chara[i].visible = r8(&b);
    }
    tmp.stage.say_speaker = read_str(&b, s);
    tmp.stage.say_text = read_str(&b, s);
    tmp.stage.reveal = (int32_t)r32(&b);
    tmp.stage.ending_id = r16(&b);
    tmp.stage.ending_kind = r8(&b);
    tmp.stage.ending_active = r8(&b);

    tmp.rng = r32(&b);
    tmp.steps = r64(&b);
    tmp.current = r8(&b);
    tmp.halted = r8(&b);
    if (b.bad) return VN_SAVE_E_TRUNCATED;
    if (tmp.current != 0xFF && tmp.current >= VN_MAX_THREADS) return VN_SAVE_E_RANGE;

    tmp.error = 0;
    tmp.error_msg = NULL;
    *m = tmp;
    return VN_SAVE_OK;
}

vn_save_status vn_profile_write(const vn_profile *pr, const vn_program *p,
                                uint8_t *out, size_t cap, size_t *written)
{
    wbuf b = { out, cap, 0, 0 };
    for (int i = 0; i < 4; i++) w8(&b, (uint8_t)VN_PROFILE_MAGIC[i]);
    w16(&b, VN_SAVE_VERSION);
    w16(&b, 0);

    uint32_t ns = 0;
    for (uint32_t i = 0; i < p->sym_count; i++)
        if (p->syms[(size_t)i * VN_SYM_REC_SIZE + 4] == VN_BANK_SF) ns++;
    w32(&b, ns);
    for (uint32_t i = 0; i < p->sym_count; i++) {
        const uint8_t *r = p->syms + (size_t)i * VN_SYM_REC_SIZE;
        if (r[4] != VN_BANK_SF) continue;
        uint16_t slot = vn_rd16(r + 6);
        w32(&b, vn_rd32(r));
        w32(&b, slot < VN_SF_SLOTS ? (uint32_t)pr->sf[slot] : 0u);
    }

    w32(&b, (uint32_t)sizeof pr->ending_seen);
    for (size_t i = 0; i < sizeof pr->ending_seen; i++) w8(&b, pr->ending_seen[i]);
    w32(&b, (uint32_t)sizeof pr->read_bits);
    for (size_t i = 0; i < sizeof pr->read_bits; i++) w8(&b, pr->read_bits[i]);

    if (b.bad) return VN_SAVE_E_NO_SPACE;
    if (written) *written = b.n;
    return VN_SAVE_OK;
}

vn_save_status vn_profile_read(vn_profile *pr, const vn_program *p,
                               const uint8_t *in, size_t n)
{
    rbuf b = { in, n, 0, 0 };
    if (n < 8) return VN_SAVE_E_TRUNCATED;
    if (memcmp(in, VN_PROFILE_MAGIC, 4) != 0) return VN_SAVE_E_MAGIC;
    b.at = 4;
    if (r16(&b) != VN_SAVE_VERSION) return VN_SAVE_E_VERSION;
    (void)r16(&b);

    static vn_profile tmp;
    memset(&tmp, 0, sizeof tmp);

    uint32_t ns = r32(&b);
    if (b.bad) return VN_SAVE_E_TRUNCATED;
    for (uint32_t i = 0; i < ns; i++) {
        uint32_t name = r32(&b);
        uint32_t val = r32(&b);
        if (b.bad) return VN_SAVE_E_TRUNCATED;
        uint8_t bank; uint16_t slot;
        if (vn_find_symbol(p, name, &bank, &slot) &&
            bank == VN_BANK_SF && slot < VN_SF_SLOTS)
            tmp.sf[slot] = (int32_t)val;
    }

    uint32_t ne = r32(&b);
    if (ne > sizeof tmp.ending_seen) return VN_SAVE_E_RANGE;
    for (uint32_t i = 0; i < ne; i++) tmp.ending_seen[i] = r8(&b);
    uint32_t nr = r32(&b);
    if (nr > sizeof tmp.read_bits) return VN_SAVE_E_RANGE;
    for (uint32_t i = 0; i < nr; i++) tmp.read_bits[i] = r8(&b);
    if (b.bad) return VN_SAVE_E_TRUNCATED;

    *pr = tmp;
    return VN_SAVE_OK;
}
