/* vn_bytecode.h - the contract between the compiler (tools/vnc) and the
 * VM (core/src/vn_vm.c). Both include this file and nothing else agrees
 * on the format.
 *
 * Design constraint, from docs/research.md part 2.6: the VM must be
 * snapshottable by a flat memcpy. That rules out anything the bytecode
 * could do to create hidden state - no closures, no allocation, no
 * host-language stack that could span a yield. The instruction set is
 * deliberately small and boring, in the tradition of the SC3 VM that
 * ran Steins;Gate.
 *
 * All multi-byte fields are little-endian, explicitly encoded, so a
 * .vnb built on any host runs on every target.
 */
#ifndef VN_BYTECODE_H
#define VN_BYTECODE_H

#include <stdint.h>
#include <stddef.h>

#define VN_VNB_MAGIC   "VNB1"
#define VN_VNSTR_MAGIC "VSTR"
#define VN_VNB_VERSION 1

/* Sentinel for "no string". Lives here rather than in vn_vm.h because
 * the compiler needs it too. */
#define VN_STR_NONE 0xFFFFu

/* Variable banks. Scope discipline is the single most transferable
 * structural fact from the research (part 2.3): route unlocks must live
 * in profile-global scope, story progress in save-local scope. Encoding
 * that as separate banks makes it impossible to get wrong by accident. */
typedef enum {
    VN_BANK_F  = 0,   /* save-local:      serialized into every save   */
    VN_BANK_SF = 1,   /* profile-global:  outlives saves; route gates  */
    VN_BANK_TF = 2,   /* transient:       never saved; scratch         */
    VN_BANK_L  = 3,   /* thread-local:    per-thread scratch           */
    VN_BANK_COUNT
} vn_bank;

typedef enum {
    VN_OP_HALT = 0,
    VN_OP_NOP,

    /* --- expression evaluation ---------------------------------
     * These are the ONLY instructions that touch the operand stack,
     * and the compiler guarantees the stack is empty at every
     * blocking instruction. The VM asserts it. */
    VN_OP_PUSH,        /* i32 imm                  -> push            */
    VN_OP_LOAD,        /* u8 bank, u16 slot        -> push            */
    VN_OP_STORE,       /* u8 bank, u16 slot        <- pop             */
    VN_OP_ADD,         /* saturating                                  */
    VN_OP_SUB,
    VN_OP_MUL,
    VN_OP_EQ, VN_OP_NE, VN_OP_LT, VN_OP_LE, VN_OP_GT, VN_OP_GE,
    VN_OP_AND, VN_OP_OR, VN_OP_NOT,

    /* --- control flow ------------------------------------------ */
    VN_OP_JMP,         /* u32 addr                                   */
    VN_OP_JZ,          /* u32 addr                 <- pop            */
    VN_OP_CALL,        /* u32 addr                                   */
    VN_OP_RET,
    VN_OP_SPAWN,       /* u32 addr - start a cooperative thread      */
    VN_OP_YIELD,
    VN_OP_END_THREAD,

    /* --- presentation ------------------------------------------
     * SAY, NARR and WAIT block. Everything else mutates the stage
     * and falls through. */
    VN_OP_SAY,         /* u16 speaker_str, u16 text_str   (blocks)   */
    VN_OP_NARR,        /* u16 text_str                    (blocks)   */
    VN_OP_BG,          /* u16 asset_str, u16 fade_ms                 */
    VN_OP_SHOW,        /* u16 asset_str, u8 slot, u8 pos             */
    VN_OP_HIDE,        /* u8 slot                                    */
    VN_OP_BGM,         /* u16 asset_str                              */
    VN_OP_SFX,         /* u16 asset_str                              */
    VN_OP_WAIT,        /* u16 ms                          (blocks)   */

    /* --- choices -----------------------------------------------
     * Split into three opcodes so that a per-option guard can be an
     * arbitrary expression while the opcodes stay fixed-shape. */
    VN_OP_CHOICE_BEGIN,
    VN_OP_CHOICE_ADD,  /* u16 text_str, u32 target  <- pop enabled   */
    VN_OP_CHOICE_SHOW, /*                                  (blocks)  */

    /* --- endings ------------------------------------------------
     * An ending is a first-class, numbered, registry-tracked node,
     * not a failure state (research part 2.3). */
    VN_OP_ENDING,      /* u16 ending_id, u8 kind                     */

    VN_OP_COUNT
} vn_op;

typedef enum {
    VN_END_BAD  = 0,
    VN_END_GOOD = 1,
    VN_END_TRUE = 2
} vn_ending_kind;

/* Byte length of each instruction's operands (opcode byte excluded). */
static const uint8_t vn_op_operand_len[VN_OP_COUNT] = {
    /* HALT */ 0, /* NOP */ 0,
    /* PUSH */ 4, /* LOAD */ 3, /* STORE */ 3,
    /* ADD */ 0, /* SUB */ 0, /* MUL */ 0,
    /* EQ */ 0, /* NE */ 0, /* LT */ 0, /* LE */ 0, /* GT */ 0, /* GE */ 0,
    /* AND */ 0, /* OR */ 0, /* NOT */ 0,
    /* JMP */ 4, /* JZ */ 4, /* CALL */ 4, /* RET */ 0,
    /* SPAWN */ 4, /* YIELD */ 0, /* END_THREAD */ 0,
    /* SAY */ 4, /* NARR */ 2, /* BG */ 4, /* SHOW */ 4, /* HIDE */ 1,
    /* BGM */ 2, /* SFX */ 2, /* WAIT */ 2,
    /* CHOICE_BEGIN */ 0, /* CHOICE_ADD */ 6, /* CHOICE_SHOW */ 0,
    /* ENDING */ 3
};

/* Instructions after which a thread stops running this frame. Used by
 * the VM scheduler and asserted against by the compiler, so the two can
 * never disagree about where a snapshot may be taken. */
static const uint8_t vn_op_blocks[VN_OP_COUNT] = {
    0, 0,
    0, 0, 0,
    0, 0, 0,
    0, 0, 0, 0, 0, 0,
    0, 0, 0,
    0, 0, 0, 0,
    0, 1 /* YIELD */, 0,
    1 /* SAY */, 1 /* NARR */, 0, 0, 0,
    0, 0, 1 /* WAIT */,
    0, 0, 1 /* CHOICE_SHOW */,
    0
};

static inline int vn_op_len(uint8_t op)
{
    if (op >= VN_OP_COUNT) return -1;
    return 1 + (int)vn_op_operand_len[op];
}

/* --- little-endian primitives ---------------------------------- */

static inline uint16_t vn_rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t vn_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void vn_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void vn_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/* FNV-1a. Used for label names, variable names and body hashes.
 * Saves address labels and variables BY NAME HASH rather than by index,
 * which is what lets a save survive edits elsewhere in the script
 * (research part 2.6: every snapshot engine surveyed breaks saves on
 * any script edit; this is the fix). */
static inline uint32_t vn_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

static inline uint32_t vn_hash_bytes(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* --- .vnb container --------------------------------------------
 *
 *   char     magic[4]      "VNB1"
 *   u16      version
 *   u16      reserved
 *   u32      script_hash        whole-program hash, for save compat
 *   u32      code_off, code_len
 *   u32      label_off, label_count
 *   u32      sym_off,   sym_count
 *   u32      slots[VN_BANK_COUNT]   highest slot+1 allocated per bank
 *
 * label record (16 bytes):
 *   u32 name_hash, u32 addr, u32 body_hash, u16 title_str, u16 reserved
 *
 * symbol record (8 bytes):
 *   u32 name_hash, u8 bank, u8 reserved, u16 slot
 */
#define VN_VNB_HEADER_SIZE   (4 + 2 + 2 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 4 * VN_BANK_COUNT)
#define VN_LABEL_REC_SIZE    16
#define VN_SYM_REC_SIZE      8

#endif /* VN_BYTECODE_H */
