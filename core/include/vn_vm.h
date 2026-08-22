/* vn_vm.h - the script virtual machine.
 *
 * Everything here is sized at compile time and lives in flat structs.
 * There is no allocation, no pointer into the heap that a snapshot
 * would have to chase, and no host-language stack that can span a
 * blocking instruction. That is the whole design: a save is a memcpy of
 * vn_machine, and it works precisely because the machine is too weak to
 * hold anything it could not describe.
 *
 * See docs/research.md part 2.6 for why this beats the alternatives.
 */
#ifndef VN_VM_H
#define VN_VM_H

#include "vn_bytecode.h"

/* --- fixed capacities -------------------------------------------
 * Every one of these bounds the snapshot. Raising them costs save
 * size and watch RAM; they are deliberately modest. */
#define VN_MAX_THREADS      8
#define VN_CALL_DEPTH      16
#define VN_THREAD_LOCALS   32
#define VN_EXPR_STACK      16
#define VN_MAX_CHOICES      8
#define VN_CHAR_SLOTS       3

#define VN_F_SLOTS       4096
#define VN_SF_SLOTS      2048
#define VN_TF_SLOTS       256

typedef enum {
    VN_TH_DEAD = 0,
    VN_TH_RUN,
    VN_TH_BLOCKED_SAY,
    VN_TH_BLOCKED_CHOICE,
    VN_TH_BLOCKED_WAIT,
    VN_TH_BLOCKED_YIELD
} vn_thread_state;

typedef struct {
    uint32_t pc;
    uint32_t wait_until;                 /* ms, for BLOCKED_WAIT */
    uint8_t  state;                      /* vn_thread_state */
    uint8_t  call_depth;
    uint8_t  sp;                         /* expression stack pointer */
    uint8_t  choice_count;
    uint32_t call_stack[VN_CALL_DEPTH];
    int32_t  local[VN_THREAD_LOCALS];
    int32_t  stack[VN_EXPR_STACK];
    uint16_t choice_text[VN_MAX_CHOICES];
    uint32_t choice_target[VN_MAX_CHOICES];
    uint8_t  choice_enabled[VN_MAX_CHOICES];
} vn_thread;

/* Presentation state lives INSIDE the machine, so restoring a save
 * restores the visuals with nothing re-executed. This is the KiriKiri
 * model (research part 2.4) and it is why loads are instant. */
typedef struct {
    uint16_t bg, bg_fade_ms;
    uint16_t bgm;
    uint16_t sfx_pending;
    struct {
        uint16_t asset;
        uint8_t  pos;                    /* 0 left, 1 center, 2 right */
        uint8_t  visible;
    } chara[VN_CHAR_SLOTS];
    uint16_t say_speaker;                /* VN_STR_NONE for narration */
    uint16_t say_text;
    int32_t  reveal;                     /* typewriter cursor, -1 = all */
    uint16_t ending_id;
    uint8_t  ending_kind;
    uint8_t  ending_active;
} vn_stage;

/* Save-local machine state. This whole struct is the snapshot. */
typedef struct {
    vn_thread thread[VN_MAX_THREADS];
    int32_t   f[VN_F_SLOTS];
    int32_t   tf[VN_TF_SLOTS];
    vn_stage  stage;
    uint32_t  rng;
    uint64_t  steps;                     /* instructions retired; makes
                                            determinism testable */
    uint8_t   current;                   /* thread owning the screen */
    uint8_t   halted;
    uint8_t   error;
    const char *error_msg;               /* static string, never owned */
} vn_machine;

/* Profile-global state: survives across saves and playthroughs. Route
 * unlocks, endings seen, read-text registry (research part 2.3). */
typedef struct {
    int32_t  sf[VN_SF_SLOTS];
    uint8_t  ending_seen[32];            /* bitset, 256 endings */
    uint8_t  read_bits[8192];            /* one bit per code address;
                                            RealLive's bitset, not KAG's
                                            one-variable-per-label */
} vn_profile;

/* A loaded .vnb. Borrows the blob; copies nothing. */
typedef struct {
    const uint8_t *blob;
    size_t         blob_len;
    const uint8_t *code;
    uint32_t       code_len;
    const uint8_t *labels;               /* label_count * VN_LABEL_REC_SIZE */
    uint32_t       label_count;
    const uint8_t *syms;                 /* sym_count * VN_SYM_REC_SIZE */
    uint32_t       sym_count;
    uint32_t       script_hash;
    uint32_t       slots[VN_BANK_COUNT];
} vn_program;

/* A loaded .vnstr string table for one locale. */
typedef struct {
    const uint8_t *blob;
    size_t         blob_len;
    uint32_t       count;
    const uint8_t *offsets;              /* count+1 u32 offsets into data */
    const uint8_t *hashes;               /* count u32 source-text hashes  */
    const char    *data;
} vn_strings;

typedef enum {
    VN_RUN_BLOCKED = 0,   /* all threads waiting on input or time */
    VN_RUN_HALTED,
    VN_RUN_ERROR
} vn_run_result;

/* --- loading ----------------------------------------------------- */

int vn_program_load(vn_program *p, const uint8_t *data, size_t n);
int vn_strings_load(vn_strings *s, const uint8_t *data, size_t n);

/* Returns "" for out-of-range ids rather than NULL, so callers never
 * have to null-check in a draw loop. */
const char *vn_str(const vn_strings *s, uint16_t id);

/* Source hash of a string, and the reverse lookup. Saves record stage
 * strings by hash rather than by id, so editing the script (which
 * renumbers ids) does not restore the wrong background. */
uint32_t vn_str_hash(const vn_strings *s, uint16_t id);
int      vn_str_by_hash(const vn_strings *s, uint32_t hash, uint16_t *id);

/* Label lookup by name hash. Returns 1 and fills addr/body_hash. */
int vn_find_label(const vn_program *p, uint32_t name_hash,
                  uint32_t *addr, uint32_t *body_hash);

/* Symbol lookup by name hash. Returns 1 and fills bank/slot. */
int vn_find_symbol(const vn_program *p, uint32_t name_hash,
                   uint8_t *bank, uint16_t *slot);

/* --- execution ---------------------------------------------------- */

void vn_machine_init(vn_machine *m, const vn_program *p, uint32_t entry_addr);
void vn_profile_init(vn_profile *pr);

/* Run every runnable thread until all block, the program halts, or
 * `budget` instructions are retired. `now_ms` drives WAIT only, and is
 * passed in rather than read from the clock so tests are deterministic. */
vn_run_result vn_run(vn_machine *m, const vn_program *p, vn_profile *pr,
                     uint32_t now_ms, uint32_t budget);

/* --- host interaction --------------------------------------------- */

void vn_advance(vn_machine *m);              /* dismiss the current line */
int  vn_choice_count(const vn_machine *m);
uint16_t vn_choice_text(const vn_machine *m, int i);
int  vn_choice_enabled(const vn_machine *m, int i);
int  vn_choose(vn_machine *m, int index);    /* 0 if index invalid/disabled */

/* --- read-text registry ------------------------------------------- */

void vn_mark_read(vn_profile *pr, uint32_t addr);
int  vn_is_read(const vn_profile *pr, uint32_t addr);

#endif /* VN_VM_H */
