/* vnfont - rasterize a TTF into a fixed-cell bitmap font.
 *
 * The engine never ships a TrueType file. A CJK TTF is 15-20 MB per
 * weight, which is a quarter of the entire watchOS bundle budget for a
 * single font (docs/research.md 3.6). Bitmap glyphs also give us the
 * hard-edged, unhinted, no-antialiasing look the PC-98 aesthetic needs,
 * for free.
 *
 * Rasterization is deliberately thresholded, not blended: a glyph pixel
 * is either on or off. Period hardware had no subpixel coverage and the
 * text layer was a 1-bit stencil.
 *
 * Output format (.vnf):
 *   "VNF1"                        4 bytes
 *   u16 cell_w, u16 cell_h        little endian
 *   u16 first_cp, u16 count
 *   glyph[count][cell_h * ((cell_w+7)/8)]   MSB-first rows
 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u16(FILE *f, unsigned v)
{
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "usage: vnfont <font.ttf> <out.vnf> <cell_h> [cell_w] [first] [count] [threshold]\n"
            "  defaults: cell_w=cell_h/2, first=32, count=95, threshold=128\n");
        return 2;
    }
    const char *in_path = argv[1];
    const char *out_path = argv[2];
    int cell_h = atoi(argv[3]);
    int cell_w = argc > 4 ? atoi(argv[4]) : cell_h / 2;
    int first  = argc > 5 ? atoi(argv[5]) : 32;
    int count  = argc > 6 ? atoi(argv[6]) : 95;
    int thresh = argc > 7 ? atoi(argv[7]) : 128;

    if (cell_h <= 0 || cell_w <= 0 || count <= 0) {
        fprintf(stderr, "vnfont: bad cell geometry\n");
        return 2;
    }

    FILE *in = fopen(in_path, "rb");
    if (!in) { fprintf(stderr, "vnfont: cannot open %s\n", in_path); return 1; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *ttf = malloc((size_t)sz);
    if (!ttf || fread(ttf, 1, (size_t)sz, in) != (size_t)sz) {
        fprintf(stderr, "vnfont: cannot read %s\n", in_path);
        return 1;
    }
    fclose(in);

    stbtt_fontinfo font;
    if (!stbtt_InitFont(&font, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
        fprintf(stderr, "vnfont: not a usable TTF\n");
        return 1;
    }

    /* Scale so the ascent+descent band fits the cell exactly. */
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &line_gap);
    float scale = stbtt_ScaleForPixelHeight(&font, (float)cell_h);
    int baseline = (int)(ascent * scale + 0.5f);

    int stride = (cell_w + 7) / 8;
    unsigned char *cell = calloc((size_t)(cell_h * stride), 1);
    unsigned char *gray = calloc((size_t)(cell_w * cell_h), 1);
    if (!cell || !gray) { fprintf(stderr, "vnfont: oom\n"); return 1; }

    FILE *out = fopen(out_path, "wb");
    if (!out) { fprintf(stderr, "vnfont: cannot write %s\n", out_path); return 1; }
    fwrite("VNF1", 1, 4, out);
    put_u16(out, (unsigned)cell_w);
    put_u16(out, (unsigned)cell_h);
    put_u16(out, (unsigned)first);
    put_u16(out, (unsigned)count);

    int drawn = 0;
    for (int i = 0; i < count; i++) {
        int cp = first + i;
        memset(cell, 0, (size_t)(cell_h * stride));
        memset(gray, 0, (size_t)(cell_w * cell_h));

        int aw, lsb;
        stbtt_GetCodepointHMetrics(&font, cp, &aw, &lsb);
        int x0, y0, x1, y1;
        stbtt_GetCodepointBitmapBox(&font, cp, scale, scale, &x0, &y0, &x1, &y1);

        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0) {
            unsigned char *tmp = calloc((size_t)(gw * gh), 1);
            if (!tmp) { fprintf(stderr, "vnfont: oom\n"); return 1; }
            stbtt_MakeCodepointBitmap(&font, tmp, gw, gh, gw, scale, scale, cp);

            /* Centre horizontally in the cell; anchor vertically to the
             * baseline so glyphs sit on a common line. */
            int ox = (cell_w - gw) / 2;
            int oy = baseline + y0;
            for (int y = 0; y < gh; y++) {
                int dy = oy + y;
                if (dy < 0 || dy >= cell_h) continue;
                for (int x = 0; x < gw; x++) {
                    int dx = ox + x;
                    if (dx < 0 || dx >= cell_w) continue;
                    gray[dy * cell_w + dx] = tmp[y * gw + x];
                }
            }
            free(tmp);
            drawn++;
        }

        /* Hard threshold: on or off, never blended. */
        for (int y = 0; y < cell_h; y++)
            for (int x = 0; x < cell_w; x++)
                if (gray[y * cell_w + x] >= thresh)
                    cell[y * stride + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));

        fwrite(cell, 1, (size_t)(cell_h * stride), out);
    }
    fclose(out);

    fprintf(stderr, "vnfont: %s -> %s  %dx%d cells, %d glyphs from U+%04X (%d with ink)\n",
            in_path, out_path, cell_w, cell_h, count, first, drawn);
    return 0;
}
