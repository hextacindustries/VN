/* vn_save.h - snapshot and restore.
 *
 * The architectural claim this file exists to prove: because the VM has
 * no registers, no locals, no closures and a bounded call stack, its
 * entire state is a value, and saving anywhere is therefore free. See
 * docs/research.md part 2.6.
 *
 * Two design choices make saves survive script edits, which every
 * engine surveyed in the research fails at:
 *
 *   1. Code addresses are stored as (label name hash, offset within
 *      that label) rather than raw offsets, together with a hash of the
 *      label's body. A save is valid as long as the scene it is sitting
 *      in is unchanged - editing a DIFFERENT scene no longer breaks it.
 *   2. Variables are stored by name hash, not bank index, so adding a
 *      new flag does not shift every existing one.
 */
#ifndef VN_SAVE_H
#define VN_SAVE_H

#include "vn_vm.h"

#define VN_SAVE_MAGIC    "VSAV"
#define VN_SAVE_VERSION  1
#define VN_PROFILE_MAGIC "VPRF"

typedef enum {
    VN_SAVE_OK = 0,
    VN_SAVE_E_MAGIC,
    VN_SAVE_E_VERSION,
    VN_SAVE_E_TRUNCATED,
    VN_SAVE_E_NO_SPACE,
    VN_SAVE_E_LABEL_MISSING,   /* the scene it was saved in is gone      */
    VN_SAVE_E_LABEL_CHANGED,   /* that scene was edited; offset unsafe   */
    VN_SAVE_E_RANGE
} vn_save_status;

const char *vn_save_status_str(vn_save_status st);

/* Upper bound on a save, so callers can size a static buffer. */
size_t vn_save_bound(void);

vn_save_status vn_save_write(const vn_machine *m, const vn_program *p,
                             const vn_strings *s,
                             uint8_t *out, size_t cap, size_t *written);

vn_save_status vn_save_read(vn_machine *m, const vn_program *p,
                            const vn_strings *s,
                            const uint8_t *in, size_t n);

/* Profile state (sf bank, endings seen, read registry) is written
 * separately: it outlives individual saves. */
vn_save_status vn_profile_write(const vn_profile *pr, const vn_program *p,
                                uint8_t *out, size_t cap, size_t *written);

vn_save_status vn_profile_read(vn_profile *pr, const vn_program *p,
                               const uint8_t *in, size_t n);

size_t vn_profile_bound(void);

#endif /* VN_SAVE_H */
