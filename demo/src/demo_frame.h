#ifndef DEMO_FRAME_H
#define DEMO_FRAME_H

#include "vn_gfx.h"
#include "vn_text.h"

/* Compose one full 640x400 scene into `s`.
 * `reveal` is the typewriter cursor, or -1 to show the whole line. */
void demo_render(vn_surface *s, const vn_font *font,
                 const char *speaker, const char *line, int reveal);

#endif
