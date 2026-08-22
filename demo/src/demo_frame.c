/* demo_frame.c - a single composed scene, drawn with nothing but the
 * core rasterizer. This is the visual smoke test for the PC-98 look:
 * dithered gradients, hard silhouettes, an 8px-aligned message window
 * and typewriter text, all in 16 colours.
 */
#include "demo_frame.h"

/* Vertical gradient dithered across a palette ramp.
 *
 * Sixteen colours cannot express a sky, so period art faked tonal range
 * with ordered dithering between adjacent ramp steps. Density is graded
 * continuously here, which is what separates hand-dithered PC-98 art
 * from Floyd-Steinberg noise (docs/research.md 1.5).
 */
static void gradient_v(vn_surface *s, int x, int y, int w, int h,
                       const uint8_t *ramp, int steps)
{
    if (steps < 2 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        int t     = (row * (steps - 1) * 64) / h;   /* 0 .. (steps-1)*64 */
        int band  = t >> 6;
        int frac  = t & 63;
        if (band >= steps - 1) { band = steps - 2; frac = 63; }
        uint8_t lo = ramp[band], hi = ramp[band + 1];
        int dy = y + row;
        if (dy < 0 || dy >= s->h) continue;
        uint8_t *dp = s->idx + (long)dy * s->w;
        for (int col = 0; col < w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= s->w) continue;
            dp[dx] = (vn_bayer8(dx, dy) < frac) ? hi : lo;
        }
    }
}

/* Filled circle, no antialiasing - the hard-edged specular shapes in
 * period art were solid, never blended. */
static void disc(vn_surface *s, int cx, int cy, int r, uint8_t color)
{
    for (int dy = -r; dy <= r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= s->h) continue;
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) span++;
        uint8_t *dp = s->idx + (long)yy * s->w;
        for (int dx = -span; dx <= span; dx++) {
            int xx = cx + dx;
            if (xx >= 0 && xx < s->w) dp[xx] = color;
        }
    }
}

/* A ragged hill ridge, drawn as solid silhouette. */
static void ridge(vn_surface *s, int base_y, int amp, int period,
                  int phase, uint8_t color)
{
    for (int x = 0; x < s->w; x++) {
        /* Cheap deterministic profile: two out-of-phase triangle waves.
         * No floating point, so this is identical on every target. */
        int a = ((x + phase) % period) * 2;
        if (a > period) a = 2 * period - a;
        int b = ((x + phase * 3) % (period / 2)) * 2;
        if (b > period / 2) b = period - b;
        int top = base_y - (a * amp) / period - (b * amp) / (2 * period);
        if (top < 0) top = 0;
        for (int y = top; y < s->h; y++) s->idx[(long)y * s->w + x] = color;
    }
}

void demo_render(vn_surface *s, const vn_font *font, const demo_view *v)
{
    /* --- sky: dusk ramp, dark at the zenith to warm at the horizon --- */
    static const uint8_t sky[] = { 1, 2, 3, 11, 14, 15 };
    gradient_v(s, 0, 0, s->w, 260, sky, (int)(sizeof sky / sizeof sky[0]));

    /* --- moon: solid, un-dithered, two palette slots.
     * Hard-edged specular shapes were never blended in period art, so
     * the maria are flat discs rather than a gradient. */
    disc(s, 486, 74, 30, 7);
    disc(s, 476, 66,  7, 6);
    disc(s, 495, 82,  5, 6);
    disc(s, 491, 61,  3, 6);

    /* --- layered silhouettes, far to near --- */
    ridge(s, 268, 46, 210, 40, 3);
    ridge(s, 292, 34, 130, 17, 2);

    /* --- a building block with lit windows --- */
    vn_fill_rect(s, 96, 176, 152, 128, 1);
    vn_frame_rect(s, 96, 176, 152, 128, 2);
    for (int wy = 0; wy < 4; wy++)
        for (int wx = 0; wx < 4; wx++) {
            int lit = ((wx * 5 + wy * 3) % 7) < 4;
            vn_fill_rect(s, 112 + wx * 32, 192 + wy * 28, 16, 16, lit ? 15 : 1);
        }

    /* --- ground: 50% checkerboard, the signature PC-98 texture --- */
    vn_fill_checker(s, 0, 304, s->w, s->h - 304, 1, 2);

    /* --- choices, when the VM is waiting on one ---------------------
     * Options are a vertical list, never positioned by pixel. That is
     * what lets a 400x400 round watch face reflow the same scene. */
    if (v->nchoices > 0) {
        int bw = 448, bx = (s->w - bw) / 2;
        int rowh = 28, by = 300 - (v->nchoices * rowh) / 2;
        for (int i = 0; i < v->nchoices; i++) {
            int y = by + i * rowh;
            int sel = (i == v->selected);
            vn_fill_rect(s, bx, y, bw, rowh - 4, sel ? 4 : 1);
            vn_frame_rect(s, bx, y, bw, rowh - 4, v->enabled[i] ? 6 : 3);
            /* A disabled option is shown, not hidden: seeing a locked
             * door is what tells the player another route exists. */
            uint8_t ink = v->enabled[i] ? (sel ? 7 : 6) : 3;
            vn_draw_text(s, font, v->choices[i] ? v->choices[i] : "",
                         bx + 12, y + 6, bw - 24, ink, -1);
        }
        return;
    }

    /* --- message window ---------------------------------------------
     * Aligned to 8px horizontally because one VRAM byte was 8 pixels in
     * one bitplane, so every period window frame landed on that grid.
     * Height is a multiple of the 16px font cell. */
    const int bx = 16, by = 288, bw = 608, bh = 96;
    vn_fill_rect(s, bx, by, bw, bh, 1);
    vn_frame_rect(s, bx, by, bw, bh, 6);
    vn_frame_rect(s, bx + 2, by + 2, bw - 4, bh - 4, 4);

    if (v->speaker && *v->speaker) {
        int nw = vn_text_length(v->speaker) * font->cell_w + 16;
        vn_fill_rect(s, bx + 16, by - 20, nw, 20, 1);
        vn_frame_rect(s, bx + 16, by - 20, nw, 20, 6);
        vn_draw_text(s, font, v->speaker, bx + 24, by - 18, nw, 13, -1);
    }

    vn_draw_text(s, font, v->line ? v->line : "",
                 bx + 16, by + 14, bw - 32, 7, v->reveal);
}
