/* vn_gfx.h - PC-98 style indexed software rasterizer.
 *
 * The engine composites entirely in 8-bit palette-index space and only
 * expands to RGB once, at present time. That is what lets the identical
 * renderer run on a Raspberry Pi, in WebAssembly and on a watch: there
 * is no GPU anywhere in the pipeline.
 *
 * Authenticity notes (see docs/research.md part 1):
 *   - Native canvas is 640x400 with SQUARE pixels. The PC-98 signal was
 *     16:10 by design; period 4:3 CRTs stretched it ~20% vertically, so
 *     that stretch is an optional display filter, never baked into art.
 *   - The palette is 16 entries of 4 bits per channel (12-bit, 4096
 *     colours). Expansion to 8-bit is value*17, which is exactly what
 *     AliceSoft's shipping VSP decoder did.
 *   - There are no alpha blends. The hardware had none, so tonal range
 *     comes from ordered dithering and transitions are dissolves driven
 *     by a Bayer threshold. This is both authentic and the only thing
 *     that works in index space.
 */
#ifndef VN_GFX_H
#define VN_GFX_H

#include <stdint.h>
#include <stddef.h>

#define VN_SCREEN_W   640
#define VN_SCREEN_H   400
#define VN_PAL_SIZE    16

/* Index reserved as "transparent" for keyed blits. Period art reserved
 * a colour slot for this rather than carrying an alpha channel. */
#define VN_KEY_INDEX    0

/* A palette entry is packed 0x0RGB, 4 bits per channel, matching the
 * PC-9801 analog palette exactly. */
typedef uint16_t vn_color;

#define VN_RGB(r, g, b) ((vn_color)((((r) & 0xF) << 8) | (((g) & 0xF) << 4) | ((b) & 0xF)))
#define VN_R(c) (((c) >> 8) & 0xF)
#define VN_G(c) (((c) >> 4) & 0xF)
#define VN_B(c) ((c) & 0xF)

typedef struct {
    uint8_t *idx;   /* w*h palette indices, row-major, no padding */
    int      w, h;
} vn_surface;

typedef struct {
    vn_color entry[VN_PAL_SIZE];
} vn_palette;

/* --- surfaces ---------------------------------------------------- */

void vn_surface_init(vn_surface *s, uint8_t *storage, int w, int h);
void vn_surface_clear(vn_surface *s, uint8_t index);
void vn_surface_copy(vn_surface *dst, const vn_surface *src);

/* --- drawing ----------------------------------------------------- */

void vn_fill_rect(vn_surface *s, int x, int y, int w, int h, uint8_t index);
void vn_frame_rect(vn_surface *s, int x, int y, int w, int h, uint8_t index);

/* Straight copy, no transparency. */
void vn_blit(vn_surface *dst, const vn_surface *src, int dx, int dy);

/* Copy skipping VN_KEY_INDEX pixels - the period "transparent colour"
 * technique used for character sprites over a background. */
void vn_blit_keyed(vn_surface *dst, const vn_surface *src, int dx, int dy);

/* --- dithering and transitions ----------------------------------- */

/* Ordered 8x8 Bayer threshold, 0..63. Exposed because both the
 * dissolve transition and any runtime shading use the same matrix. */
uint8_t vn_bayer8(int x, int y);

/* Fill a rect with a 50% checkerboard of two indices - the single most
 * characteristic PC-98 texture. */
void vn_fill_checker(vn_surface *s, int x, int y, int w, int h,
                     uint8_t a, uint8_t b);

/* Dissolve `src` into `dst` by an ordered-dither threshold.
 * progress runs 0 (nothing) .. 64 (fully replaced). This is how the
 * hardware faded between screens, and it is exact in index space. */
void vn_dissolve(vn_surface *dst, const vn_surface *src, int progress);

/* --- presentation ------------------------------------------------ */

/* Expand indices to XRGB8888 for vn_plat_blit(). `out` must hold
 * s->w * s->h uint32_t. This is the only place RGB exists. */
void vn_palette_expand(const vn_surface *s, const vn_palette *pal, uint32_t *out);

/* A default 16-colour palette in period idiom: a neutral ramp plus the
 * cyan/magenta/purple-leaning accents the 12-bit space makes cheap. */
void vn_palette_default(vn_palette *pal);

#endif /* VN_GFX_H */
