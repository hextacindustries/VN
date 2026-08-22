#ifndef DEMO_FRAME_H
#define DEMO_FRAME_H

#include "vn_gfx.h"
#include "vn_text.h"
#include "vn_vm.h"

/* Everything the renderer needs for one frame, resolved from the VM's
 * stage state. The renderer never touches the VM directly: the stage is
 * the whole interface, which is what will let the watch and browser
 * shells draw the same scene very differently. */
typedef struct {
    const char *speaker;                    /* NULL for narration      */
    const char *line;
    int         reveal;                     /* typewriter cursor, -1 all */
    const char *choices[VN_MAX_CHOICES];
    int         enabled[VN_MAX_CHOICES];
    int         nchoices;
    int         selected;
} demo_view;

void demo_render(vn_surface *s, const vn_font *font, const demo_view *v);

#endif
