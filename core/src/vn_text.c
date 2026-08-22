#include "vn_text.h"

static unsigned rd16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }

int vn_font_load(vn_font *f, const uint8_t *data, size_t n)
{
    if (n < 12) return 0;
    if (data[0] != 'V' || data[1] != 'N' || data[2] != 'F' || data[3] != '1') return 0;

    f->cell_w = (int)rd16(data + 4);
    f->cell_h = (int)rd16(data + 6);
    f->first  = (int)rd16(data + 8);
    f->count  = (int)rd16(data + 10);
    if (f->cell_w <= 0 || f->cell_h <= 0 || f->count <= 0) return 0;

    f->stride = (f->cell_w + 7) / 8;
    size_t need = (size_t)f->count * (size_t)f->cell_h * (size_t)f->stride;
    if (n - 12 < need) return 0;

    f->bits = data + 12;
    return 1;
}

void vn_draw_char(vn_surface *s, const vn_font *f, int cp,
                  int x, int y, uint8_t color)
{
    int gi = cp - f->first;
    if (gi < 0 || gi >= f->count) return;

    const uint8_t *g = f->bits + (size_t)gi * f->cell_h * f->stride;
    for (int row = 0; row < f->cell_h; row++) {
        int dy = y + row;
        if (dy < 0 || dy >= s->h) continue;
        const uint8_t *rp = g + (size_t)row * f->stride;
        uint8_t *dp = s->idx + (long)dy * s->w;
        for (int col = 0; col < f->cell_w; col++) {
            int dx = x + col;
            if (dx < 0 || dx >= s->w) continue;
            if (rp[col >> 3] & (0x80 >> (col & 7))) dp[dx] = color;
        }
    }
}

/* Length in characters of the word starting at p (up to the next space,
 * newline or terminator). */
static int word_len(const char *p)
{
    int n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '\n') n++;
    return n;
}

/* Shared layout walk used by both drawing and measuring. When `s` is
 * NULL nothing is drawn and only the line count is produced. Keeping
 * one implementation means measurement can never disagree with what
 * actually lands on screen. */
static int layout(vn_surface *s, const vn_font *f, const char *text,
                  int x, int y, int max_w, uint8_t color, int reveal)
{
    int cw = f->cell_w, ch = f->cell_h;
    int cols = max_w / cw;
    if (cols < 1) cols = 1;

    int line = 0, col = 0, shown = 0;
    const char *p = text;

    while (*p) {
        if (*p == '\n') { line++; col = 0; p++; continue; }

        if (*p == ' ') {
            if (reveal >= 0 && shown >= reveal) return line + 1;
            /* A space never starts a line: one left over from a wrap is
             * simply dropped. Otherwise it consumes a cell. */
            if (col > 0) {
                if (col + 1 >= cols) { line++; col = 0; }
                else col++;
            }
            shown++;
            p++;
            continue;
        }

        int wl = word_len(p);
        /* Wrap before the word if it does not fit and the line has
         * content. A word longer than the whole line hard-breaks. */
        if (wl <= cols && col + wl > cols) { line++; col = 0; }

        for (int i = 0; i < wl; i++) {
            if (col >= cols) { line++; col = 0; }
            if (reveal >= 0 && shown >= reveal) return line + 1;
            if (s) vn_draw_char(s, f, (unsigned char)p[i],
                                x + col * cw, y + line * ch, color);
            col++;
            shown++;
        }
        p += wl;
    }
    return line + 1;
}

int vn_draw_text(vn_surface *s, const vn_font *f, const char *text,
                 int x, int y, int max_w, uint8_t color, int reveal)
{
    return layout(s, f, text, x, y, max_w, color, reveal);
}

int vn_text_lines(const vn_font *f, const char *text, int max_w)
{
    return layout(0, f, text, 0, 0, max_w, 0, -1);
}

int vn_text_length(const char *text)
{
    int n = 0;
    for (const char *p = text; *p; p++) if (*p != '\n') n++;
    return n;
}
