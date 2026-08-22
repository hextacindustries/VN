/* vn_text.h - bitmap font loading and text layout.
 *
 * Word wrapping, line breaking and auto-fit are ENGINE features, not
 * localisation afterthoughts. Every legacy VN engine surveyed in
 * docs/research.md got this wrong: KiriKiri "doesn't support automatic
 * word wrapping since it was written solely for Japanese", and
 * ONScripter-EN and Ponscripter exist almost entirely to bolt it on.
 * English runs 1.5-2x the box width of the same Japanese text, so no
 * layout here may ever be length-critical.
 */
#ifndef VN_TEXT_H
#define VN_TEXT_H

#include "vn_gfx.h"

typedef struct {
    int            cell_w, cell_h;
    int            first, count;   /* codepoint range covered */
    int            stride;         /* bytes per glyph row */
    const uint8_t *bits;           /* count * cell_h * stride, MSB first */
} vn_font;

/* Parse a .vnf produced by tools/vnfont. Borrows `data`; copies nothing.
 * Returns 1 on success, 0 if the blob is malformed or truncated. */
int vn_font_load(vn_font *f, const uint8_t *data, size_t n);

/* Draw one glyph. Codepoints outside the font's range draw nothing. */
void vn_draw_char(vn_surface *s, const vn_font *f, int cp,
                  int x, int y, uint8_t color);

/* Draw wrapped text into a box of `max_w` pixels starting at (x, y).
 *
 * `reveal` is the typewriter cursor: draw only the first `reveal`
 * visible characters, or -1 for all of them. Wrapping is computed over
 * the WHOLE string regardless of `reveal`, so revealing a character
 * never reflows text that is already on screen.
 *
 * Returns the number of lines laid out. */
int vn_draw_text(vn_surface *s, const vn_font *f, const char *text,
                 int x, int y, int max_w, uint8_t color, int reveal);

/* Same wrapping, but measures instead of drawing. */
int vn_text_lines(const vn_font *f, const char *text, int max_w);

/* Total count of drawable (non-newline) characters - the upper bound
 * for a typewriter reveal. */
int vn_text_length(const char *text);

#endif /* VN_TEXT_H */
