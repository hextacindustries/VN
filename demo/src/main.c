/* demo main - renders scene frames through the headless port.
 *
 * Every buffer here is static. The core allocates nothing after init by
 * design (docs/research.md 3.4), which is what keeps a future watchOS
 * or bare-metal port honest.
 */
#include "demo_frame.h"
#include "vn_platform.h"

#include <stdio.h>
#include <string.h>

void vn_headless_set_output(const char *path);

static uint8_t  g_fore_px[VN_SCREEN_W * VN_SCREEN_H];
static uint8_t  g_back_px[VN_SCREEN_W * VN_SCREEN_H];
static uint32_t g_rgb[VN_SCREEN_W * VN_SCREEN_H];
static uint8_t  g_font_blob[64 * 1024];

int main(int argc, char **argv)
{
    const char *font_path = argc > 1 ? argv[1] : "demo/assets/font8x16.vnf";
    const char *out_dir   = argc > 2 ? argv[2] : ".";

    void *fh = vn_plat_file_open(font_path);
    if (!fh) { fprintf(stderr, "cannot open font %s\n", font_path); return 1; }
    size_t fn = vn_plat_file_read(fh, g_font_blob, sizeof g_font_blob);
    vn_plat_file_close(fh);

    vn_font font;
    if (!vn_font_load(&font, g_font_blob, fn)) {
        fprintf(stderr, "malformed font %s\n", font_path);
        return 1;
    }

    vn_surface fore, back;
    vn_surface_init(&fore, g_fore_px, VN_SCREEN_W, VN_SCREEN_H);
    vn_surface_init(&back, g_back_px, VN_SCREEN_W, VN_SCREEN_H);

    vn_palette pal;
    vn_palette_default(&pal);

    const char *speaker = "SUMIRE";
    const char *line =
        "The rain had stopped sometime before dawn, and the gate stood "
        "open the way it always did. I had walked this road every "
        "morning for eight years without once looking up at it.";

    char path[512];

    /* --- typewriter reveal, four stops --------------------------- */
    int total = vn_text_length(line);
    const int stops[] = { 24, 80, 150, -1 };
    for (int i = 0; i < 4; i++) {
        vn_surface_clear(&fore, 0);
        demo_render(&fore, &font, speaker, line, stops[i]);
        vn_palette_expand(&fore, &pal, g_rgb);
        snprintf(path, sizeof path, "%s/frame_reveal_%d.ppm", out_dir, i);
        vn_headless_set_output(path);
        vn_plat_blit(g_rgb, fore.w, fore.h);
    }

    /* --- dissolve transition, the period-authentic crossfade -----
     * Alpha blending does not exist in index space, so the engine does
     * what the hardware did: an ordered-dither dissolve. */
    vn_surface_clear(&back, 0);
    demo_render(&back, &font, speaker, line, -1);

    vn_surface_clear(&fore, 3);
    vn_fill_checker(&fore, 0, 0, fore.w, fore.h, 2, 3);

    const int progress[] = { 0, 16, 32, 48, 64 };
    for (int i = 0; i < 5; i++) {
        uint8_t tmp[VN_SCREEN_W * VN_SCREEN_H];
        memcpy(tmp, g_fore_px, sizeof tmp);
        vn_surface work;
        vn_surface_init(&work, tmp, VN_SCREEN_W, VN_SCREEN_H);
        vn_dissolve(&work, &back, progress[i]);
        vn_palette_expand(&work, &pal, g_rgb);
        snprintf(path, sizeof path, "%s/frame_dissolve_%d.ppm", out_dir, i);
        vn_headless_set_output(path);
        vn_plat_blit(g_rgb, work.w, work.h);
    }

    fprintf(stderr, "rendered 9 frames (%d chars of dialogue) to %s\n",
            total, out_dir);
    return 0;
}
