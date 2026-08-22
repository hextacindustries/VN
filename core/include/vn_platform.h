/* vn_platform.h - the entire platform surface of the VN engine.
 *
 * Every port implements exactly these functions. Nothing else in core/
 * touches the host. Keeping this list short is the whole portability
 * strategy: desktop, Raspberry Pi, browser (Emscripten), Wear OS
 * (NDK), watchOS (C static lib behind Swift) and bare-metal all
 * differ only here.
 *
 * Rules for implementors:
 *   - vn_plat_blit() receives a palette-expanded XRGB8888 buffer. The
 *     core has already done indexed->RGB expansion, so a port only has
 *     to hand a rectangle of pixels to the display.
 *   - No function may block for longer than a frame.
 *   - No function may allocate on behalf of the core.
 */
#ifndef VN_PLATFORM_H
#define VN_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

/* --- display ---------------------------------------------------- */

/* Present one fully composed frame. `pixels` is w*h XRGB8888 values,
 * row-major, no padding. Valid only for the duration of the call. */
void vn_plat_blit(const uint32_t *pixels, int w, int h);

/* Nominal scale factor the host would like (1 = native 640x400).
 * Ports that letterbox or reflow may return 1 and handle it locally. */
int vn_plat_scale(void);

/* --- audio ------------------------------------------------------ */

/* Queue interleaved stereo signed 16-bit samples at 44100 Hz.
 * `frames` is sample-pairs, so the buffer holds frames*2 int16 values. */
void vn_plat_audio_push(const int16_t *samples, int frames);

/* --- time ------------------------------------------------------- */

/* Monotonic milliseconds since process start. Must never go backwards. */
uint32_t vn_plat_now_ms(void);

/* --- input ------------------------------------------------------ */

typedef enum {
    VN_INPUT_NONE = 0,
    VN_INPUT_ADVANCE,   /* tap / click / A / space - the only required one */
    VN_INPUT_UP,
    VN_INPUT_DOWN,
    VN_INPUT_CONFIRM,
    VN_INPUT_CANCEL,
    VN_INPUT_SKIP,
    VN_INPUT_AUTO,
    VN_INPUT_BACKLOG,
    VN_INPUT_QUIT
} vn_input;

/* Pop the next pending input event, or VN_INPUT_NONE if the queue is
 * empty. The core drains this every frame. */
vn_input vn_plat_poll_input(void);

/* --- files (read-only asset access) ------------------------------ */

/* Open an asset by logical path (e.g. "script/day01.vnb"). Returns an
 * opaque handle or NULL. Ports back this with a real filesystem, an
 * archive, or a baked-in blob. */
void *vn_plat_file_open(const char *path);
size_t vn_plat_file_size(void *handle);
size_t vn_plat_file_read(void *handle, void *dst, size_t n);
void vn_plat_file_close(void *handle);

/* --- saves (read-write, small) ----------------------------------- */

int vn_plat_save_write(const char *slot, const void *data, size_t n);
size_t vn_plat_save_read(const char *slot, void *dst, size_t cap);

/* --- misc -------------------------------------------------------- */

void vn_plat_log(const char *msg);
void vn_plat_exit(int code);

#endif /* VN_PLATFORM_H */
