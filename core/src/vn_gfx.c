#include "vn_gfx.h"

/* No allocation anywhere in this file. Every entry point writes into
 * caller-owned storage, which is what makes the renderer usable on a
 * microcontroller and keeps snapshot sizes fixed. */

static int clip_span(int dst_extent, int *dpos, int *spos, int *len)
{
    if (*dpos < 0) { int off = -*dpos; *spos += off; *len -= off; *dpos = 0; }
    if (*dpos + *len > dst_extent) *len = dst_extent - *dpos;
    return *len > 0;
}

void vn_surface_init(vn_surface *s, uint8_t *storage, int w, int h)
{
    s->idx = storage;
    s->w = w;
    s->h = h;
}

void vn_surface_clear(vn_surface *s, uint8_t index)
{
    long n = (long)s->w * s->h;
    for (long i = 0; i < n; i++) s->idx[i] = index;
}

void vn_surface_copy(vn_surface *dst, const vn_surface *src)
{
    int h = dst->h < src->h ? dst->h : src->h;
    int w = dst->w < src->w ? dst->w : src->w;
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src->idx + (long)y * src->w;
        uint8_t *dp = dst->idx + (long)y * dst->w;
        for (int x = 0; x < w; x++) dp[x] = sp[x];
    }
}

void vn_fill_rect(vn_surface *s, int x, int y, int w, int h, uint8_t index)
{
    int sx = 0, sy = 0;
    if (!clip_span(s->w, &x, &sx, &w)) return;
    if (!clip_span(s->h, &y, &sy, &h)) return;
    for (int row = 0; row < h; row++) {
        uint8_t *dp = s->idx + (long)(y + row) * s->w + x;
        for (int col = 0; col < w; col++) dp[col] = index;
    }
}

void vn_frame_rect(vn_surface *s, int x, int y, int w, int h, uint8_t index)
{
    if (w <= 0 || h <= 0) return;
    vn_fill_rect(s, x, y, w, 1, index);
    vn_fill_rect(s, x, y + h - 1, w, 1, index);
    vn_fill_rect(s, x, y, 1, h, index);
    vn_fill_rect(s, x + w - 1, y, 1, h, index);
}

void vn_blit(vn_surface *dst, const vn_surface *src, int dx, int dy)
{
    int sx = 0, sy = 0, w = src->w, h = src->h;
    if (!clip_span(dst->w, &dx, &sx, &w)) return;
    if (!clip_span(dst->h, &dy, &sy, &h)) return;
    for (int row = 0; row < h; row++) {
        const uint8_t *sp = src->idx + (long)(sy + row) * src->w + sx;
        uint8_t *dp = dst->idx + (long)(dy + row) * dst->w + dx;
        for (int col = 0; col < w; col++) dp[col] = sp[col];
    }
}

void vn_blit_keyed(vn_surface *dst, const vn_surface *src, int dx, int dy)
{
    int sx = 0, sy = 0, w = src->w, h = src->h;
    if (!clip_span(dst->w, &dx, &sx, &w)) return;
    if (!clip_span(dst->h, &dy, &sy, &h)) return;
    for (int row = 0; row < h; row++) {
        const uint8_t *sp = src->idx + (long)(sy + row) * src->w + sx;
        uint8_t *dp = dst->idx + (long)(dy + row) * dst->w + dx;
        for (int col = 0; col < w; col++)
            if (sp[col] != VN_KEY_INDEX) dp[col] = sp[col];
    }
}

/* Standard 8x8 ordered-dither matrix, values 0..63. */
static const uint8_t bayer8[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21
};

uint8_t vn_bayer8(int x, int y)
{
    return bayer8[((y & 7) << 3) | (x & 7)];
}

void vn_fill_checker(vn_surface *s, int x, int y, int w, int h,
                     uint8_t a, uint8_t b)
{
    int sx = 0, sy = 0;
    if (!clip_span(s->w, &x, &sx, &w)) return;
    if (!clip_span(s->h, &y, &sy, &h)) return;
    for (int row = 0; row < h; row++) {
        uint8_t *dp = s->idx + (long)(y + row) * s->w + x;
        for (int col = 0; col < w; col++)
            dp[col] = (((x + col) ^ (y + row)) & 1) ? b : a;
    }
}

void vn_dissolve(vn_surface *dst, const vn_surface *src, int progress)
{
    if (progress <= 0) return;
    int h = dst->h < src->h ? dst->h : src->h;
    int w = dst->w < src->w ? dst->w : src->w;
    if (progress >= 64) {
        for (int y = 0; y < h; y++) {
            const uint8_t *sp = src->idx + (long)y * src->w;
            uint8_t *dp = dst->idx + (long)y * dst->w;
            for (int x = 0; x < w; x++) dp[x] = sp[x];
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *sp = src->idx + (long)y * src->w;
        uint8_t *dp = dst->idx + (long)y * dst->w;
        const uint8_t *brow = bayer8 + ((y & 7) << 3);
        for (int x = 0; x < w; x++)
            if (brow[x & 7] < progress) dp[x] = sp[x];
    }
}

void vn_palette_expand(const vn_surface *s, const vn_palette *pal, uint32_t *out)
{
    /* Precompute the 16-entry lookup once per frame; the inner loop is
     * then a single indexed load. At 640x400 this is ~256k lookups,
     * roughly 0.3-2ms even on a Cortex-A53. */
    uint32_t lut[VN_PAL_SIZE];
    for (int i = 0; i < VN_PAL_SIZE; i++) {
        vn_color c = pal->entry[i];
        /* 4-bit -> 8-bit is *17, i.e. 0xF -> 0xFF. Confirmed against
         * AliceSoft's VSP decoder; see docs/research.md 1.2. */
        uint32_t r = (uint32_t)VN_R(c) * 17u;
        uint32_t g = (uint32_t)VN_G(c) * 17u;
        uint32_t b = (uint32_t)VN_B(c) * 17u;
        lut[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    long n = (long)s->w * s->h;
    for (long i = 0; i < n; i++) out[i] = lut[s->idx[i] & (VN_PAL_SIZE - 1)];
}

void vn_palette_default(vn_palette *pal)
{
    static const vn_color d[VN_PAL_SIZE] = {
        VN_RGB(0x0, 0x0, 0x1),  /*  0 key / near-black ink            */
        VN_RGB(0x1, 0x1, 0x2),  /*  1 deep shadow                     */
        VN_RGB(0x2, 0x2, 0x4),  /*  2 shadow, blue-shifted            */
        VN_RGB(0x4, 0x3, 0x6),  /*  3 mid shadow, purple-shifted      */
        VN_RGB(0x6, 0x5, 0x8),  /*  4 mid                             */
        VN_RGB(0x9, 0x8, 0xA),  /*  5 light mid                       */
        VN_RGB(0xC, 0xC, 0xD),  /*  6 light                           */
        VN_RGB(0xF, 0xF, 0xF),  /*  7 white / specular highlight      */
        VN_RGB(0xF, 0xD, 0xB),  /*  8 skin highlight                  */
        VN_RGB(0xE, 0xB, 0x9),  /*  9 skin base                       */
        VN_RGB(0xB, 0x7, 0x7),  /* 10 skin shadow, magenta-shifted    */
        VN_RGB(0x8, 0x4, 0x6),  /* 11 skin deep shadow                */
        VN_RGB(0x2, 0x8, 0x9),  /* 12 teal accent                     */
        VN_RGB(0x4, 0xC, 0xC),  /* 13 cyan accent                     */
        VN_RGB(0x9, 0x3, 0x8),  /* 14 magenta accent                  */
        VN_RGB(0xD, 0x6, 0x3)   /* 15 warm accent / lamplight         */
    };
    for (int i = 0; i < VN_PAL_SIZE; i++) pal->entry[i] = d[i];
}
