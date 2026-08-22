/* vnc - compile the VN script DSL to .vnb bytecode + .vnstr strings.
 *
 * Two things this compiler must get right, both learned from
 * docs/research.md:
 *
 *  1. Display text NEVER lands in the bytecode. Every string is
 *     extracted into a per-locale table with a stable id and a hash of
 *     the source text. RLdev had this in 2004; Fate/stay night had only
 *     a convention (@texton/@textoff) and its translators paid for that
 *     forever.
 *
 *  2. The expression stack must be empty at every blocking instruction.
 *     Codegen never emits a blocking opcode mid-expression, and the VM
 *     asserts it. That invariant is what makes save-anywhere a memcpy.
 *
 * `vnc -d` decompiles .vnb back to readable source. Shipping the
 * decompiler is deliberate: RealLive got an open-source reimplementation
 * because Haeleth built a lossless round trip; Siglus did not document
 * its bytecode and its games are correspondingly unportable.
 */
#include "vn_bytecode.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* diagnostics                                                         */
/* ------------------------------------------------------------------ */

static const char *g_src_name = "<input>";
static int g_errors = 0;

static void err_at(int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "%s:%d: error: ", g_src_name, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_errors++;
}

/* ------------------------------------------------------------------ */
/* lexer                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    T_EOF, T_NEWLINE, T_IDENT, T_INT, T_STRING,
    T_ARROW,        /* -> */
    T_OP            /* single/double char operator */
} tok_kind;

typedef struct {
    tok_kind kind;
    char     text[256];
    long     ival;
    int      line;
} token;

/* One identifier length everywhere, so a long name can never be
 * silently truncated on its way between tables. */
#define VNC_NAME_MAX 256

#define MAX_TOKENS 200000
static token g_tok[MAX_TOKENS];
static int   g_ntok = 0;
static int   g_pos = 0;

static void add_tok(tok_kind k, const char *s, long v, int line)
{
    if (g_ntok >= MAX_TOKENS) { fprintf(stderr, "vnc: token limit\n"); exit(1); }
    token *t = &g_tok[g_ntok++];
    t->kind = k;
    t->ival = v;
    t->line = line;
    if (s) { snprintf(t->text, sizeof t->text, "%s", s); }
    else t->text[0] = 0;
}

static int is_ident_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.';
}

/* `narr` and speaker lines take the rest of the line as prose, so the
 * lexer needs to know when to stop tokenizing and grab raw text. */
static int line_starts_with_narr(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "narr", 4) == 0 && (p[4] == ' ' || p[4] == '\t');
}

static void lex(const char *src)
{
    const char *p = src;
    int line = 1;

    while (*p) {
        /* start of line: check for the narr special case */
        const char *ls = p;
        if (line_starts_with_narr(ls)) {
            while (*p == ' ' || *p == '\t') p++;
            add_tok(T_IDENT, "narr", 0, line);
            p += 4;
            while (*p == ' ' || *p == '\t') p++;
            const char *start = p;
            while (*p && *p != '\n') p++;
            /* trim trailing whitespace */
            const char *e = p;
            while (e > start && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
            char buf[1024];
            size_t n = (size_t)(e - start);
            if (n >= sizeof buf) n = sizeof buf - 1;
            memcpy(buf, start, n);
            buf[n] = 0;
            add_tok(T_STRING, buf, 0, line);
            continue;
        }

        while (*p && *p != '\n') {
            if (*p == ' ' || *p == '\t' || *p == '\r') { p++; continue; }
            if (*p == '#') { while (*p && *p != '\n') p++; break; }

            if (*p == '"') {
                p++;
                char buf[1024];
                size_t n = 0;
                while (*p && *p != '"' && *p != '\n') {
                    if (*p == '\\' && p[1]) {
                        p++;
                        char c = *p++;
                        if (n + 1 < sizeof buf)
                            buf[n++] = (c == 'n') ? '\n' : c;
                        continue;
                    }
                    if (n + 1 < sizeof buf) buf[n++] = *p;
                    p++;
                }
                if (*p != '"') { err_at(line, "unterminated string"); }
                else p++;
                buf[n] = 0;
                add_tok(T_STRING, buf, 0, line);
                continue;
            }

            if (*p == '-' && p[1] == '>') { add_tok(T_ARROW, "->", 0, line); p += 2; continue; }

            if (*p >= '0' && *p <= '9') {
                long v = 0;
                const char *s = p;
                while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
                char buf[64];
                size_t n = (size_t)(p - s);
                if (n >= sizeof buf) n = sizeof buf - 1;
                memcpy(buf, s, n); buf[n] = 0;
                add_tok(T_INT, buf, v, line);
                continue;
            }

            if (is_ident_char(*p) && !(*p >= '0' && *p <= '9')) {
                const char *s = p;
                while (is_ident_char(*p)) p++;
                char buf[256];
                size_t n = (size_t)(p - s);
                if (n >= sizeof buf) n = sizeof buf - 1;
                memcpy(buf, s, n); buf[n] = 0;
                add_tok(T_IDENT, buf, 0, line);
                continue;
            }

            /* operators */
            {
                char buf[3] = { p[0], 0, 0 };
                if ((p[0] == '=' || p[0] == '!' || p[0] == '<' || p[0] == '>') && p[1] == '=') {
                    buf[1] = '=';
                    p += 2;
                } else p += 1;
                add_tok(T_OP, buf, 0, line);
            }
        }

        if (*p == '\n') { add_tok(T_NEWLINE, "\\n", 0, line); p++; line++; }
    }
    add_tok(T_EOF, "", 0, line);
}

/* ------------------------------------------------------------------ */
/* tables: strings, symbols, labels, endings                           */
/* ------------------------------------------------------------------ */

#define MAX_STRINGS 20000
static char    *g_str[MAX_STRINGS];
static uint32_t g_str_hash[MAX_STRINGS];
static int      g_nstr = 0;

static int intern_string(const char *s)
{
    for (int i = 0; i < g_nstr; i++)
        if (strcmp(g_str[i], s) == 0) return i;
    if (g_nstr >= MAX_STRINGS) { fprintf(stderr, "vnc: string limit\n"); exit(1); }
    g_str[g_nstr] = malloc(strlen(s) + 1);
    strcpy(g_str[g_nstr], s);
    g_str_hash[g_nstr] = vn_hash(s);
    return g_nstr++;
}

#define MAX_SYMS 4096
typedef struct { char name[VNC_NAME_MAX]; uint8_t bank; uint16_t slot; } sym;
static sym g_sym[MAX_SYMS];
static int g_nsym = 0;
static uint32_t g_next_slot[VN_BANK_COUNT];

static int sym_lookup(const char *name, uint8_t *bank, uint16_t *slot, int line)
{
    uint8_t b;
    const char *base;
    if (strncmp(name, "sf.", 3) == 0)      { b = VN_BANK_SF; base = name; }
    else if (strncmp(name, "tf.", 3) == 0) { b = VN_BANK_TF; base = name; }
    else if (strncmp(name, "f.", 2) == 0)  { b = VN_BANK_F;  base = name; }
    else { err_at(line, "variable '%s' must start with f. sf. or tf.", name); return 0; }

    for (int i = 0; i < g_nsym; i++)
        if (strcmp(g_sym[i].name, base) == 0) {
            *bank = g_sym[i].bank; *slot = g_sym[i].slot; return 1;
        }

    if (g_nsym >= MAX_SYMS) { fprintf(stderr, "vnc: symbol limit\n"); exit(1); }
    sym *s = &g_sym[g_nsym++];
    snprintf(s->name, sizeof s->name, "%s", base);
    s->bank = b;
    s->slot = (uint16_t)g_next_slot[b]++;
    *bank = s->bank; *slot = s->slot;
    return 1;
}

#define MAX_LABELS 8192
typedef struct { char name[VNC_NAME_MAX]; uint32_t addr; uint16_t title; int defined; int line; } label;
static label g_label[MAX_LABELS];
static int   g_nlabel = 0;

static label *label_find_or_add(const char *name)
{
    for (int i = 0; i < g_nlabel; i++)
        if (strcmp(g_label[i].name, name) == 0) return &g_label[i];
    if (g_nlabel >= MAX_LABELS) { fprintf(stderr, "vnc: label limit\n"); exit(1); }
    label *l = &g_label[g_nlabel++];
    snprintf(l->name, sizeof l->name, "%s", name);
    l->addr = 0; l->title = VN_STR_NONE; l->defined = 0; l->line = 0;
    return l;
}

#define MAX_ENDINGS 256
static char g_ending[MAX_ENDINGS][VNC_NAME_MAX];
static int  g_nending = 0;

static int ending_id(const char *name)
{
    for (int i = 0; i < g_nending; i++)
        if (strcmp(g_ending[i], name) == 0) return i;
    if (g_nending >= MAX_ENDINGS) { fprintf(stderr, "vnc: ending limit\n"); exit(1); }
    snprintf(g_ending[g_nending], sizeof g_ending[0], "%s", name);
    return g_nending++;
}

/* Character name -> stage slot, assigned in order of first appearance.
 * Fixed slots keep the stage struct small and the snapshot flat; it is
 * also how period engines avoided "which sprite is this" bugs at scale. */
#define VNC_CHARA_SLOTS 3
static char g_chara[VNC_CHARA_SLOTS][VNC_NAME_MAX];
static int  g_nchara = 0;

static int chara_slot(const char *name, int line)
{
    for (int i = 0; i < g_nchara; i++)
        if (strcmp(g_chara[i], name) == 0) return i;
    if (g_nchara >= VNC_CHARA_SLOTS) {
        err_at(line, "more than %d characters on stage at once", VNC_CHARA_SLOTS);
        return 0;
    }
    snprintf(g_chara[g_nchara], sizeof g_chara[0], "%s", name);
    return g_nchara++;
}

static int pos_code(const char *name, int line)
{
    if (strcmp(name, "left") == 0)   return 0;
    if (strcmp(name, "center") == 0) return 1;
    if (strcmp(name, "right") == 0)  return 2;
    err_at(line, "unknown position '%s' (want left, center or right)", name);
    return 1;
}

/* ------------------------------------------------------------------ */
/* code buffer                                                         */
/* ------------------------------------------------------------------ */

#define MAX_CODE (1 << 20)
static uint8_t  g_code[MAX_CODE];
static uint32_t g_ncode = 0;

static void emit8(uint8_t v)
{
    if (g_ncode + 1 > MAX_CODE) { fprintf(stderr, "vnc: code limit\n"); exit(1); }
    g_code[g_ncode++] = v;
}
static void emit16(uint16_t v) { emit8((uint8_t)(v & 0xFF)); emit8((uint8_t)(v >> 8)); }
static void emit32(uint32_t v) { emit16((uint16_t)(v & 0xFFFF)); emit16((uint16_t)(v >> 16)); }

/* Forward references to labels are patched after the whole file is
 * parsed, so scripts can jump forwards freely. */
#define MAX_FIXUPS 20000
typedef struct { uint32_t pos; char name[VNC_NAME_MAX]; int line; } fixup;
static fixup g_fix[MAX_FIXUPS];
static int   g_nfix = 0;

static void emit_label_ref(const char *name, int line)
{
    if (g_nfix >= MAX_FIXUPS) { fprintf(stderr, "vnc: fixup limit\n"); exit(1); }
    fixup *f = &g_fix[g_nfix++];
    f->pos = g_ncode;
    f->line = line;
    snprintf(f->name, sizeof f->name, "%s", name);
    emit32(0);
}

/* ------------------------------------------------------------------ */
/* parser plumbing                                                     */
/* ------------------------------------------------------------------ */

static token *peek(void)  { return &g_tok[g_pos]; }
static token *next_tok(void) { return &g_tok[g_pos++]; }

static void skip_newlines(void)
{
    while (g_tok[g_pos].kind == T_NEWLINE) g_pos++;
}

static int at_ident(const char *s)
{
    return g_tok[g_pos].kind == T_IDENT && strcmp(g_tok[g_pos].text, s) == 0;
}

static int accept_ident(const char *s)
{
    if (at_ident(s)) { g_pos++; return 1; }
    return 0;
}

static int at_op(const char *s)
{
    return g_tok[g_pos].kind == T_OP && strcmp(g_tok[g_pos].text, s) == 0;
}

static int accept_op(const char *s)
{
    if (at_op(s)) { g_pos++; return 1; }
    return 0;
}

static token *expect(tok_kind k, const char *what)
{
    if (g_tok[g_pos].kind != k) {
        err_at(g_tok[g_pos].line, "expected %s, found '%s'", what, g_tok[g_pos].text);
        return &g_tok[g_pos];
    }
    return &g_tok[g_pos++];
}

static void end_of_line(void)
{
    if (g_tok[g_pos].kind == T_NEWLINE) { g_pos++; return; }
    if (g_tok[g_pos].kind == T_EOF) return;
    err_at(g_tok[g_pos].line, "unexpected '%s' at end of statement", g_tok[g_pos].text);
    while (g_tok[g_pos].kind != T_NEWLINE && g_tok[g_pos].kind != T_EOF) g_pos++;
    if (g_tok[g_pos].kind == T_NEWLINE) g_pos++;
}

/* ------------------------------------------------------------------ */
/* expressions - emitted postfix, never containing a blocking opcode    */
/* ------------------------------------------------------------------ */

static void parse_expr(void);

static void parse_atom(void)
{
    token *t = peek();
    if (accept_op("(")) { parse_expr(); if (!accept_op(")")) err_at(t->line, "missing ')'"); return; }
    if (t->kind == T_INT)  { next_tok(); emit8(VN_OP_PUSH); emit32((uint32_t)t->ival); return; }
    if (t->kind == T_IDENT) {
        next_tok();
        uint8_t bank; uint16_t slot;
        if (sym_lookup(t->text, &bank, &slot, t->line)) {
            emit8(VN_OP_LOAD); emit8(bank); emit16(slot);
        } else { emit8(VN_OP_PUSH); emit32(0); }
        return;
    }
    err_at(t->line, "expected a value, found '%s'", t->text);
    next_tok();
    emit8(VN_OP_PUSH); emit32(0);
}

static void parse_mul(void)
{
    parse_atom();
    while (at_op("*")) { next_tok(); parse_atom(); emit8(VN_OP_MUL); }
}

static void parse_add(void)
{
    parse_mul();
    for (;;) {
        if (accept_op("+")) { parse_mul(); emit8(VN_OP_ADD); }
        else if (accept_op("-")) { parse_mul(); emit8(VN_OP_SUB); }
        else break;
    }
}

static void parse_cmp(void)
{
    parse_add();
    static const struct { const char *op; uint8_t code; } ops[] = {
        { "==", VN_OP_EQ }, { "!=", VN_OP_NE }, { "<=", VN_OP_LE },
        { ">=", VN_OP_GE }, { "<", VN_OP_LT }, { ">", VN_OP_GT }
    };
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++)
        if (accept_op(ops[i].op)) { parse_add(); emit8(ops[i].code); return; }
}

static void parse_not(void)
{
    if (accept_ident("not")) { parse_not(); emit8(VN_OP_NOT); return; }
    parse_cmp();
}

static void parse_and(void)
{
    parse_not();
    while (accept_ident("and")) { parse_not(); emit8(VN_OP_AND); }
}

static void parse_expr(void)
{
    parse_and();
    while (accept_ident("or")) { parse_and(); emit8(VN_OP_OR); }
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

static void parse_block(const char *ender1, const char *ender2);

static void parse_if(void)
{
    /* if <expr> ... [elif <expr> ...] [else ...] end
     * Each arm jumps to a shared exit; exits are patched directly since
     * they are all backward-known by the time the block closes. */
    uint32_t exit_fix[64];
    int nexit = 0;

    for (;;) {
        parse_expr();
        end_of_line();
        emit8(VN_OP_JZ);
        uint32_t else_fix = g_ncode;
        emit32(0);

        parse_block("elif", "else");

        int has_more = at_ident("elif") || at_ident("else");
        if (has_more) {
            emit8(VN_OP_JMP);
            if (nexit < 64) exit_fix[nexit++] = g_ncode;
            emit32(0);
        }
        vn_wr32(g_code + else_fix, g_ncode);

        if (accept_ident("elif")) continue;
        if (accept_ident("else")) {
            end_of_line();
            parse_block("end", NULL);
            if (!accept_ident("end")) err_at(peek()->line, "missing 'end' for if");
            end_of_line();
            break;
        }
        if (!accept_ident("end")) err_at(peek()->line, "missing 'end' for if");
        end_of_line();
        break;
    }

    for (int i = 0; i < nexit; i++) vn_wr32(g_code + exit_fix[i], g_ncode);
}

static void parse_choice(void)
{
    end_of_line();
    emit8(VN_OP_CHOICE_BEGIN);
    skip_newlines();

    while (!at_ident("end") && peek()->kind != T_EOF) {
        token *txt = expect(T_STRING, "a choice caption");
        int sid = intern_string(txt->text);

        if (peek()->kind == T_ARROW) next_tok();
        else err_at(peek()->line, "expected '->' after a choice caption");

        token *target = expect(T_IDENT, "a target label");
        char tname[VNC_NAME_MAX];
        snprintf(tname, sizeof tname, "%s", target->text);

        /* Optional guard. Emitted BEFORE choice_add so the enabled flag
         * is on the stack when the opcode pops it - and the stack is
         * empty again before choice_show blocks. */
        if (accept_ident("if")) parse_expr();
        else { emit8(VN_OP_PUSH); emit32(1); }

        emit8(VN_OP_CHOICE_ADD);
        emit16((uint16_t)sid);
        emit_label_ref(tname, target->line);
        end_of_line();
        skip_newlines();
    }

    if (!accept_ident("end")) err_at(peek()->line, "missing 'end' for choice");
    end_of_line();
    emit8(VN_OP_CHOICE_SHOW);
}

static int parse_stmt(void)
{
    token *t = peek();
    int line = t->line;

    if (t->kind == T_NEWLINE) { g_pos++; return 1; }
    if (t->kind == T_EOF) return 0;
    if (t->kind != T_IDENT) { err_at(line, "unexpected '%s'", t->text); end_of_line(); return 1; }

    if (accept_ident("bg")) {
        token *a = expect(T_IDENT, "a background name");
        int sid = intern_string(a->text);
        uint16_t fade = 0;
        if (accept_ident("fade")) fade = (uint16_t)expect(T_INT, "a fade duration")->ival;
        emit8(VN_OP_BG); emit16((uint16_t)sid); emit16(fade);
        end_of_line(); return 1;
    }

    if (accept_ident("show")) {
        token *who = expect(T_IDENT, "a character name");
        char asset[VNC_NAME_MAX * 2];   /* holds "name_pose" */
        int slot = chara_slot(who->text, line);
        if (peek()->kind == T_IDENT && !at_ident("at"))
            snprintf(asset, sizeof asset, "%s_%s", who->text, next_tok()->text);
        else
            snprintf(asset, sizeof asset, "%s", who->text);
        int pos = 1;
        if (accept_ident("at")) pos = pos_code(expect(T_IDENT, "a position")->text, line);
        emit8(VN_OP_SHOW); emit16((uint16_t)intern_string(asset));
        emit8((uint8_t)slot); emit8((uint8_t)pos);
        end_of_line(); return 1;
    }

    if (accept_ident("hide")) {
        token *who = expect(T_IDENT, "a character name");
        emit8(VN_OP_HIDE); emit8((uint8_t)chara_slot(who->text, line));
        end_of_line(); return 1;
    }

    if (accept_ident("bgm")) {
        token *a = expect(T_IDENT, "a music name");
        emit8(VN_OP_BGM); emit16((uint16_t)intern_string(a->text));
        end_of_line(); return 1;
    }

    if (accept_ident("sfx")) {
        token *a = expect(T_IDENT, "a sound name");
        emit8(VN_OP_SFX); emit16((uint16_t)intern_string(a->text));
        end_of_line(); return 1;
    }

    if (accept_ident("narr")) {
        token *a = expect(T_STRING, "narration text");
        emit8(VN_OP_NARR); emit16((uint16_t)intern_string(a->text));
        end_of_line(); return 1;
    }

    if (accept_ident("wait")) {
        emit8(VN_OP_WAIT); emit16((uint16_t)expect(T_INT, "a duration in ms")->ival);
        end_of_line(); return 1;
    }

    if (accept_ident("jump")) {
        emit8(VN_OP_JMP); emit_label_ref(expect(T_IDENT, "a label")->text, line);
        end_of_line(); return 1;
    }
    if (accept_ident("call")) {
        emit8(VN_OP_CALL); emit_label_ref(expect(T_IDENT, "a label")->text, line);
        end_of_line(); return 1;
    }
    if (accept_ident("spawn")) {
        emit8(VN_OP_SPAWN); emit_label_ref(expect(T_IDENT, "a label")->text, line);
        end_of_line(); return 1;
    }
    if (accept_ident("return")) { emit8(VN_OP_RET); end_of_line(); return 1; }
    if (accept_ident("yield"))  { emit8(VN_OP_YIELD); end_of_line(); return 1; }
    if (accept_ident("halt"))   { emit8(VN_OP_HALT); end_of_line(); return 1; }

    if (accept_ident("set")) {
        token *v = expect(T_IDENT, "a variable");
        char name[VNC_NAME_MAX]; snprintf(name, sizeof name, "%s", v->text);
        if (!accept_op("=")) err_at(line, "expected '=' in set");
        parse_expr();
        uint8_t bank; uint16_t slot;
        if (sym_lookup(name, &bank, &slot, line)) { emit8(VN_OP_STORE); emit8(bank); emit16(slot); }
        end_of_line(); return 1;
    }

    if (accept_ident("add")) {
        token *v = expect(T_IDENT, "a variable");
        char name[VNC_NAME_MAX]; snprintf(name, sizeof name, "%s", v->text);
        uint8_t bank; uint16_t slot;
        if (!sym_lookup(name, &bank, &slot, line)) { end_of_line(); return 1; }
        emit8(VN_OP_LOAD); emit8(bank); emit16(slot);
        parse_expr();
        emit8(VN_OP_ADD);
        emit8(VN_OP_STORE); emit8(bank); emit16(slot);
        end_of_line(); return 1;
    }

    if (accept_ident("if"))     { parse_if(); return 1; }
    if (accept_ident("choice")) { parse_choice(); return 1; }

    if (accept_ident("ending")) {
        token *nm = expect(T_IDENT, "an ending name");
        int id = ending_id(nm->text);
        uint8_t kind = VN_END_BAD;
        if (accept_ident("kind")) {
            if (!accept_op("=")) err_at(line, "expected '=' after kind");
            token *k = expect(T_IDENT, "BAD, GOOD or TRUE");
            if (strcmp(k->text, "GOOD") == 0) kind = VN_END_GOOD;
            else if (strcmp(k->text, "TRUE") == 0) kind = VN_END_TRUE;
            else if (strcmp(k->text, "BAD") != 0)
                err_at(k->line, "unknown ending kind '%s'", k->text);
        }
        emit8(VN_OP_ENDING); emit16((uint16_t)id); emit8(kind);
        end_of_line(); return 1;
    }

    /* Anything else that is IDENT followed by STRING is a speaker line. */
    if (g_tok[g_pos].kind == T_IDENT && g_tok[g_pos + 1].kind == T_STRING) {
        token *who = next_tok();
        token *txt = next_tok();
        emit8(VN_OP_SAY);
        emit16((uint16_t)intern_string(who->text));
        emit16((uint16_t)intern_string(txt->text));
        end_of_line(); return 1;
    }

    err_at(line, "unknown statement '%s'", t->text);
    end_of_line();
    return 1;
}

static void parse_block(const char *ender1, const char *ender2)
{
    for (;;) {
        skip_newlines();
        if (peek()->kind == T_EOF) return;
        if (at_ident("end")) return;
        if (ender1 && at_ident(ender1)) return;
        if (ender2 && at_ident(ender2)) return;
        if (!parse_stmt()) return;
    }
}

static void parse_scene(void)
{
    int line = peek()->line;
    token *nm = expect(T_IDENT, "a scene name");
    label *l = label_find_or_add(nm->text);
    if (l->defined) err_at(line, "scene '%s' already defined at line %d", nm->text, l->line);
    l->defined = 1;
    l->line = line;
    l->addr = g_ncode;
    if (peek()->kind == T_STRING) l->title = (uint16_t)intern_string(next_tok()->text);
    end_of_line();

    parse_block(NULL, NULL);

    if (!accept_ident("end")) err_at(peek()->line, "missing 'end' for scene '%s'", nm->text);
    end_of_line();

    /* Falling off the end of a scene ends the thread rather than
     * running into the next scene by accident. */
    emit8(VN_OP_END_THREAD);
}

/* ------------------------------------------------------------------ */
/* linking and output                                                  */
/* ------------------------------------------------------------------ */

static void resolve_fixups(void)
{
    for (int i = 0; i < g_nfix; i++) {
        label *l = NULL;
        for (int j = 0; j < g_nlabel; j++)
            if (strcmp(g_label[j].name, g_fix[i].name) == 0) { l = &g_label[j]; break; }
        if (!l || !l->defined) {
            err_at(g_fix[i].line, "jump to undefined scene '%s'", g_fix[i].name);
            continue;
        }
        vn_wr32(g_code + g_fix[i].pos, l->addr);
    }
}

/* Hash of a label's own body, NORMALIZED.
 *
 * A save records which label it sits in plus this hash; on load the
 * hash must still match or the saved offset would point at nonsense.
 * Hashing the raw bytes is wrong, because two things shift them without
 * changing the scene at all:
 *
 *   - string ids are assigned in order of first appearance, so adding a
 *     line ANYWHERE earlier renumbers every later string;
 *   - jump operands are absolute addresses, which move whenever any
 *     earlier scene changes size.
 *
 * So the hash covers structure only: opcodes, plain numeric operands,
 * and jump targets expressed as (label, offset) pairs. String operands
 * are excluded entirely - retranslating a line must not invalidate
 * anyone's save. */
static void norm_addr(uint32_t addr, uint32_t *name_hash, uint32_t *offset)
{
    int best = -1;
    for (int i = 0; i < g_nlabel; i++)
        if (g_label[i].defined && g_label[i].addr <= addr &&
            (best < 0 || g_label[i].addr >= g_label[best].addr)) best = i;
    if (best < 0) { *name_hash = 0; *offset = addr; return; }
    *name_hash = vn_hash(g_label[best].name);
    *offset = addr - g_label[best].addr;
}

static uint32_t fnv_step(uint32_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

static uint32_t fnv_u32(uint32_t h, uint32_t v)
{
    uint8_t b[4];
    vn_wr32(b, v);
    return fnv_step(h, b, 4);
}

static uint32_t label_body_hash(int idx)
{
    uint32_t start = g_label[idx].addr;
    uint32_t end = g_ncode;
    for (int i = 0; i < g_nlabel; i++)
        if (g_label[i].defined && g_label[i].addr > start && g_label[i].addr < end)
            end = g_label[i].addr;

    uint32_t h = 2166136261u;
    uint32_t pc = start;
    while (pc < end) {
        uint8_t op = g_code[pc];
        int len = vn_op_len(op);
        if (len < 0 || pc + (uint32_t)len > end) break;
        const uint8_t *a = g_code + pc + 1;
        h = fnv_step(h, &op, 1);

        switch (op) {
        case VN_OP_JMP: case VN_OP_JZ: case VN_OP_CALL: case VN_OP_SPAWN: {
            uint32_t nh, off;
            norm_addr(vn_rd32(a), &nh, &off);
            h = fnv_u32(h, nh); h = fnv_u32(h, off);
            break;
        }
        case VN_OP_CHOICE_ADD: {
            uint32_t nh, off;                 /* text id skipped */
            norm_addr(vn_rd32(a + 2), &nh, &off);
            h = fnv_u32(h, nh); h = fnv_u32(h, off);
            break;
        }
        case VN_OP_SAY: case VN_OP_NARR:
        case VN_OP_BGM: case VN_OP_SFX:
            break;                            /* text only; excluded */
        case VN_OP_BG:
            h = fnv_u32(h, vn_rd16(a + 2));   /* fade, not the asset id */
            break;
        case VN_OP_SHOW:
            h = fnv_step(h, a + 2, 2);        /* slot and position */
            break;
        case VN_OP_LOAD: case VN_OP_STORE: {
            /* Hash the variable's NAME, not its slot index. Slots shift
             * whenever a variable is added earlier in the script, and
             * saves remap by name anyway. */
            uint8_t bank = a[0];
            uint16_t slot = vn_rd16(a + 1);
            uint32_t nh = 0;
            for (int i = 0; i < g_nsym; i++)
                if (g_sym[i].bank == bank && g_sym[i].slot == slot) {
                    nh = vn_hash(g_sym[i].name);
                    break;
                }
            h = fnv_step(h, &bank, 1);
            h = fnv_u32(h, nh ? nh : (uint32_t)slot);
            break;
        }
        default:
            h = fnv_step(h, a, (size_t)vn_op_operand_len[op]);
            break;
        }
        pc += (uint32_t)len;
    }
    return h;
}

static int write_vnb(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "vnc: cannot write %s\n", path); return 0; }

    uint32_t code_off  = VN_VNB_HEADER_SIZE;
    uint32_t label_off = code_off + g_ncode;
    uint32_t sym_off   = label_off + (uint32_t)g_nlabel * VN_LABEL_REC_SIZE;

    uint8_t hdr[VN_VNB_HEADER_SIZE];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, VN_VNB_MAGIC, 4);
    vn_wr16(hdr + 4, VN_VNB_VERSION);
    vn_wr16(hdr + 6, 0);
    uint8_t *h = hdr + 8;
    vn_wr32(h + 0, vn_hash_bytes(g_code, g_ncode));
    vn_wr32(h + 4, code_off);
    vn_wr32(h + 8, g_ncode);
    vn_wr32(h + 12, label_off);
    vn_wr32(h + 16, (uint32_t)g_nlabel);
    vn_wr32(h + 20, sym_off);
    vn_wr32(h + 24, (uint32_t)g_nsym);
    for (int i = 0; i < VN_BANK_COUNT; i++) vn_wr32(h + 28 + 4 * i, g_next_slot[i]);
    fwrite(hdr, 1, sizeof hdr, f);

    fwrite(g_code, 1, g_ncode, f);

    for (int i = 0; i < g_nlabel; i++) {
        uint8_t rec[VN_LABEL_REC_SIZE];
        memset(rec, 0, sizeof rec);
        vn_wr32(rec + 0, vn_hash(g_label[i].name));
        vn_wr32(rec + 4, g_label[i].addr);
        vn_wr32(rec + 8, label_body_hash(i));
        vn_wr16(rec + 12, g_label[i].title);
        fwrite(rec, 1, sizeof rec, f);
    }

    for (int i = 0; i < g_nsym; i++) {
        uint8_t rec[VN_SYM_REC_SIZE];
        memset(rec, 0, sizeof rec);
        vn_wr32(rec + 0, vn_hash(g_sym[i].name));
        rec[4] = g_sym[i].bank;
        vn_wr16(rec + 6, g_sym[i].slot);
        fwrite(rec, 1, sizeof rec, f);
    }

    fclose(f);
    return 1;
}

/* .vnstr carries a hash of each ORIGINAL string alongside the text, so
 * a translation tool can flag entries whose source changed since the
 * translation was made. */
static int write_vnstr(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "vnc: cannot write %s\n", path); return 0; }

    fwrite(VN_VNSTR_MAGIC, 1, 4, f);
    uint8_t buf[4];
    vn_wr32(buf, (uint32_t)g_nstr); fwrite(buf, 1, 4, f);

    uint32_t off = 0;
    for (int i = 0; i < g_nstr; i++) {
        vn_wr32(buf, off); fwrite(buf, 1, 4, f);
        off += (uint32_t)strlen(g_str[i]) + 1;
    }
    vn_wr32(buf, off); fwrite(buf, 1, 4, f);

    for (int i = 0; i < g_nstr; i++) { vn_wr32(buf, g_str_hash[i]); fwrite(buf, 1, 4, f); }

    for (int i = 0; i < g_nstr; i++) fwrite(g_str[i], 1, strlen(g_str[i]) + 1, f);

    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* decompiler                                                          */
/* ------------------------------------------------------------------ */

static const char *op_name(uint8_t op)
{
    static const char *n[VN_OP_COUNT] = {
        "halt","nop","push","load","store","add","sub","mul",
        "eq","ne","lt","le","gt","ge","and","or","not",
        "jmp","jz","call","ret","spawn","yield","end_thread",
        "say","narr","bg","show","hide","bgm","sfx","wait",
        "choice_begin","choice_add","choice_show","ending"
    };
    return op < VN_OP_COUNT ? n[op] : "?";
}

static const char *bank_name(uint8_t b)
{
    switch (b) {
    case VN_BANK_F: return "f"; case VN_BANK_SF: return "sf";
    case VN_BANK_TF: return "tf"; case VN_BANK_L: return "local";
    default: return "?";
    }
}

static int decompile(const char *vnb_path, const char *vnstr_path)
{
    FILE *f = fopen(vnb_path, "rb");
    if (!f) { fprintf(stderr, "vnc: cannot open %s\n", vnb_path); return 1; }
    static uint8_t blob[8 << 20];
    size_t n = fread(blob, 1, sizeof blob, f);
    fclose(f);

    if (n < VN_VNB_HEADER_SIZE || memcmp(blob, VN_VNB_MAGIC, 4) != 0) {
        fprintf(stderr, "vnc: %s is not a .vnb\n", vnb_path);
        return 1;
    }

    static uint8_t sblob[8 << 20];
    size_t sn = 0;
    if (vnstr_path) {
        FILE *sf = fopen(vnstr_path, "rb");
        if (sf) { sn = fread(sblob, 1, sizeof sblob, sf); fclose(sf); }
    }
    uint32_t scount = 0;
    const uint8_t *soff = NULL;
    const char *sdata = NULL;
    if (sn > 8 && memcmp(sblob, VN_VNSTR_MAGIC, 4) == 0) {
        scount = vn_rd32(sblob + 4);
        soff = sblob + 8;
        sdata = (const char *)(sblob + 8 + (size_t)(scount + 1) * 4 + (size_t)scount * 4);
    }

    const uint8_t *h = blob + 8;
    uint32_t code_off = vn_rd32(h + 4), code_len = vn_rd32(h + 8);
    uint32_t label_off = vn_rd32(h + 12), label_cnt = vn_rd32(h + 16);
    const uint8_t *code = blob + code_off;

    printf("# decompiled from %s\n", vnb_path);
    printf("# %u bytes of code, %u labels, %u strings\n\n", code_len, label_cnt, scount);

    for (uint32_t pc = 0; pc < code_len; ) {
        for (uint32_t i = 0; i < label_cnt; i++) {
            const uint8_t *r = blob + label_off + (size_t)i * VN_LABEL_REC_SIZE;
            if (vn_rd32(r + 4) == pc) printf("\nlabel_%08x:            # body hash %08x\n",
                                             pc, vn_rd32(r + 8));
        }

        uint8_t op = code[pc];
        int len = vn_op_len(op);
        if (len < 0 || pc + (uint32_t)len > code_len) {
            printf("  %04x: <bad opcode %02x>\n", pc, op);
            break;
        }
        const uint8_t *a = code + pc + 1;
        printf("  %04x: %-13s", pc, op_name(op));

        switch (op) {
        case VN_OP_PUSH:  printf(" %d", (int32_t)vn_rd32(a)); break;
        case VN_OP_LOAD:
        case VN_OP_STORE: printf(" %s[%u]", bank_name(a[0]), vn_rd16(a + 1)); break;
        case VN_OP_JMP: case VN_OP_JZ: case VN_OP_CALL: case VN_OP_SPAWN:
            printf(" -> %04x", vn_rd32(a)); break;
        case VN_OP_SAY:
            if (sdata) printf(" %s: \"%s\"",
                              sdata + vn_rd32(soff + 4 * vn_rd16(a)),
                              sdata + vn_rd32(soff + 4 * vn_rd16(a + 2)));
            else printf(" str%u str%u", vn_rd16(a), vn_rd16(a + 2));
            break;
        case VN_OP_NARR: case VN_OP_BGM: case VN_OP_SFX:
            if (sdata) printf(" \"%s\"", sdata + vn_rd32(soff + 4 * vn_rd16(a)));
            else printf(" str%u", vn_rd16(a));
            break;
        case VN_OP_BG:
            if (sdata) printf(" \"%s\" fade %u", sdata + vn_rd32(soff + 4 * vn_rd16(a)), vn_rd16(a + 2));
            else printf(" str%u fade %u", vn_rd16(a), vn_rd16(a + 2));
            break;
        case VN_OP_SHOW:
            if (sdata) printf(" \"%s\" slot %u pos %u",
                              sdata + vn_rd32(soff + 4 * vn_rd16(a)), a[2], a[3]);
            else printf(" str%u slot %u pos %u", vn_rd16(a), a[2], a[3]);
            break;
        case VN_OP_HIDE:  printf(" slot %u", a[0]); break;
        case VN_OP_WAIT:  printf(" %u ms", vn_rd16(a)); break;
        case VN_OP_CHOICE_ADD:
            if (sdata) printf(" \"%s\" -> %04x", sdata + vn_rd32(soff + 4 * vn_rd16(a)), vn_rd32(a + 2));
            else printf(" str%u -> %04x", vn_rd16(a), vn_rd32(a + 2));
            break;
        case VN_OP_ENDING: printf(" id %u kind %u", vn_rd16(a), a[2]); break;
        default: break;
        }
        printf("\n");
        pc += (uint32_t)len;
    }
    return 0;
}

/* ------------------------------------------------------------------ */

static int usage(void)
{
    fprintf(stderr,
        "usage: vnc <in.vn> <out.vnb> <out.vnstr>\n"
        "       vnc -d <in.vnb> [in.vnstr]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-d") == 0)
        return decompile(argv[2], argc > 3 ? argv[3] : NULL);
    if (argc < 4) return usage();

    g_src_name = argv[1];
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "vnc: cannot open %s\n", argv[1]); return 1; }
    static char src[4 << 20];
    size_t n = fread(src, 1, sizeof src - 1, f);
    src[n] = 0;
    fclose(f);

    lex(src);

    skip_newlines();
    while (peek()->kind != T_EOF) {
        if (accept_ident("scene")) parse_scene();
        else {
            err_at(peek()->line, "expected 'scene', found '%s'", peek()->text);
            while (peek()->kind != T_NEWLINE && peek()->kind != T_EOF) g_pos++;
        }
        skip_newlines();
    }

    resolve_fixups();

    if (g_errors) {
        fprintf(stderr, "vnc: %d error(s), nothing written\n", g_errors);
        return 1;
    }

    if (!write_vnb(argv[2])) return 1;
    if (!write_vnstr(argv[3])) return 1;

    fprintf(stderr, "vnc: %s -> %u bytes code, %d labels, %d strings, %d vars\n",
            argv[1], g_ncode, g_nlabel, g_nstr, g_nsym);
    return 0;
}
