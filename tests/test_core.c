/* Core unit tests. No framework: the engine has no dependencies and
 * neither do its tests. */
#include "vn_gfx.h"
#include "vn_text.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK(cond, ...) do {                                        \
    if (!(cond)) {                                                   \
        printf("  FAIL %s:%d: ", __FILE__, __LINE__);                \
        printf(__VA_ARGS__); printf("\n");                           \
        g_fail++;                                                    \
    }                                                                \
} while (0)

static uint8_t px[64 * 32];
static uint8_t px2[64 * 32];

static void test_palette_expansion(void)
{
    /* 4-bit -> 8-bit must be *17 so 0xF maps to 0xFF exactly. This is
     * the AliceSoft VSP rule; getting it wrong washes out every colour. */
    vn_surface s;
    vn_surface_init(&s, px, 4, 1);
    s.idx[0] = 0; s.idx[1] = 1; s.idx[2] = 2; s.idx[3] = 3;

    vn_palette pal;
    memset(&pal, 0, sizeof pal);
    pal.entry[0] = VN_RGB(0xF, 0xF, 0xF);
    pal.entry[1] = VN_RGB(0x0, 0x0, 0x0);
    pal.entry[2] = VN_RGB(0x8, 0x4, 0x2);
    pal.entry[3] = VN_RGB(0xF, 0x0, 0x0);

    uint32_t out[4];
    vn_palette_expand(&s, &pal, out);

    CHECK(out[0] == 0xFFFFFFFFu, "white expanded to %08x, want FFFFFFFF", out[0]);
    CHECK(out[1] == 0xFF000000u, "black expanded to %08x, want FF000000", out[1]);
    CHECK(out[2] == 0xFF884422u, "mid expanded to %08x, want FF884422", out[2]);
    CHECK(out[3] == 0xFFFF0000u, "red expanded to %08x, want FFFF0000", out[3]);
}

static void test_blit_clipping(void)
{
    vn_surface dst, src;
    vn_surface_init(&dst, px, 16, 16);
    vn_surface_init(&src, px2, 8, 8);
    vn_surface_clear(&dst, 0);
    vn_surface_clear(&src, 5);

    /* Fully off each edge: must be a no-op, not a corruption. */
    vn_blit(&dst, &src, -100, 0);
    vn_blit(&dst, &src, 100, 0);
    vn_blit(&dst, &src, 0, -100);
    vn_blit(&dst, &src, 0, 100);
    for (int i = 0; i < 16 * 16; i++)
        CHECK(dst.idx[i] == 0, "off-screen blit wrote at %d", i);

    /* Straddling the top-left corner: an 8x8 source at (-4,-4) leaves
     * exactly the 4x4 block (0,0)..(3,3) on screen. */
    vn_blit(&dst, &src, -4, -4);
    CHECK(dst.idx[0] == 5, "corner blit missed the overlap");
    CHECK(dst.idx[3 * 16 + 3] == 5, "corner blit short of the overlap edge");
    CHECK(dst.idx[4 * 16 + 4] == 0, "corner blit overran the overlap");
    CHECK(dst.idx[3 * 16 + 4] == 0, "corner blit overran horizontally");
}

static void test_keyed_blit(void)
{
    vn_surface dst, src;
    vn_surface_init(&dst, px, 16, 16);
    vn_surface_init(&src, px2, 8, 8);
    vn_surface_clear(&dst, 9);
    vn_surface_clear(&src, VN_KEY_INDEX);
    src.idx[0] = 3;

    vn_blit_keyed(&dst, &src, 0, 0);
    CHECK(dst.idx[0] == 3, "keyed blit dropped an opaque pixel");
    CHECK(dst.idx[1] == 9, "keyed blit wrote a transparent pixel");
}

static void test_dissolve_endpoints(void)
{
    vn_surface a, b;
    vn_surface_init(&a, px, 16, 16);
    vn_surface_init(&b, px2, 16, 16);
    vn_surface_clear(&a, 1);
    vn_surface_clear(&b, 2);

    vn_dissolve(&a, &b, 0);
    for (int i = 0; i < 16 * 16; i++)
        CHECK(a.idx[i] == 1, "progress 0 changed pixel %d", i);

    vn_dissolve(&a, &b, 64);
    for (int i = 0; i < 16 * 16; i++)
        CHECK(a.idx[i] == 2, "progress 64 left pixel %d untouched", i);

    /* Halfway must be a true 50% ordered dither: exactly half the 8x8
     * Bayer cell crosses the threshold. */
    vn_surface_clear(&a, 1);
    vn_dissolve(&a, &b, 32);
    int changed = 0;
    for (int i = 0; i < 16 * 16; i++) if (a.idx[i] == 2) changed++;
    CHECK(changed == 128, "50%% dissolve changed %d of 256 pixels", changed);
}

/* A font blob built in memory, so the test needs no asset on disk. */
static uint8_t fontblob[12 + 2 * 16];

static void build_font(void)
{
    memcpy(fontblob, "VNF1", 4);
    fontblob[4] = 8;  fontblob[5] = 0;    /* cell_w = 8  */
    fontblob[6] = 16; fontblob[7] = 0;    /* cell_h = 16 */
    fontblob[8] = 'A'; fontblob[9] = 0;   /* first = 'A' */
    fontblob[10] = 2; fontblob[11] = 0;   /* count = 2   */
    for (int i = 0; i < 32; i++) fontblob[12 + i] = 0xFF;
}

static void test_font_validation(void)
{
    build_font();
    vn_font f;

    CHECK(vn_font_load(&f, fontblob, sizeof fontblob) == 1, "valid font rejected");
    CHECK(f.cell_w == 8 && f.cell_h == 16, "font geometry misparsed");
    CHECK(f.stride == 1, "stride for an 8px cell should be 1, got %d", f.stride);

    CHECK(vn_font_load(&f, fontblob, 8) == 0, "truncated header accepted");
    CHECK(vn_font_load(&f, fontblob, sizeof fontblob - 1) == 0,
          "truncated glyph data accepted");

    uint8_t bad[sizeof fontblob];
    memcpy(bad, fontblob, sizeof bad);
    bad[0] = 'X';
    CHECK(vn_font_load(&f, bad, sizeof bad) == 0, "bad magic accepted");
}

static void test_text_layout(void)
{
    build_font();
    vn_font f;
    vn_font_load(&f, fontblob, sizeof fontblob);

    /* Measurement and drawing share one implementation, so they must
     * never disagree. 10 cells of 8px = 80px wide. */
    vn_surface s;
    vn_surface_init(&s, px, 64, 32);
    vn_surface_clear(&s, 0);

    const char *t = "AAA AAA AAA AAA";
    int measured = vn_text_lines(&f, t, 80);
    int drawn = vn_draw_text(&s, &f, t, 0, 0, 80, 1, -1);
    CHECK(measured == drawn, "measured %d lines, drew %d", measured, drawn);

    /* Explicit newlines are honoured. */
    CHECK(vn_text_lines(&f, "A\nA\nA", 80) == 3, "newlines not counted");

    /* A word longer than the line must hard-break rather than overflow. */
    CHECK(vn_text_lines(&f, "AAAAAAAAAAAAAAA", 80) == 2, "long word did not break");

    /* Revealing a prefix must not reflow what is already placed. */
    int full = vn_text_lines(&f, t, 80);
    for (int r = 0; r <= vn_text_length(t); r++) {
        int partial = vn_draw_text(0, &f, t, 0, 0, 80, 1, r);
        CHECK(partial <= full, "reveal %d produced %d lines, more than the full %d",
              r, partial, full);
    }

    CHECK(vn_text_length("AB\nCD") == 4, "length should ignore newlines");
}

int main(void)
{
    printf("core tests\n");
    test_palette_expansion();
    test_blit_clipping();
    test_keyed_blit();
    test_dissolve_endpoints();
    test_font_validation();
    test_text_layout();

    if (g_fail == 0) { printf("  all passed\n"); return 0; }
    printf("  %d failure(s)\n", g_fail);
    return 1;
}
