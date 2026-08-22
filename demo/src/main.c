/* demo main - drives the compiled script through the VM and renders
 * whatever the stage says, one frame per beat.
 *
 * Nothing here knows what the story is. It loads bytecode, steps the
 * machine, reads vn_stage, and draws. Replacing this file with an SDL
 * or watchOS shell changes only how frames are presented and how input
 * arrives.
 */
#include "demo_frame.h"
#include "vn_platform.h"
#include "vn_save.h"

#include <stdio.h>
#include <string.h>

void vn_headless_set_output(const char *path);

static uint8_t  g_fore_px[VN_SCREEN_W * VN_SCREEN_H];
static uint32_t g_rgb[VN_SCREEN_W * VN_SCREEN_H];
static uint8_t  g_font_blob[64 * 1024];
static uint8_t  g_code_blob[1 << 20];
static uint8_t  g_str_blob[1 << 20];

static vn_machine m;
static vn_profile prof;
static vn_program prog;
static vn_strings strs;

static size_t load_file(const char *path, uint8_t *dst, size_t cap)
{
    void *h = vn_plat_file_open(path);
    if (!h) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    size_t n = vn_plat_file_read(h, dst, cap);
    vn_plat_file_close(h);
    return n;
}

/* Build the render view from the VM's stage. */
static void view_from_stage(demo_view *v)
{
    memset(v, 0, sizeof *v);
    v->reveal = m.stage.reveal;
    v->speaker = m.stage.say_speaker == VN_STR_NONE ? NULL : vn_str(&strs, m.stage.say_speaker);
    v->line = vn_str(&strs, m.stage.say_text);

    v->nchoices = vn_choice_count(&m);
    for (int i = 0; i < v->nchoices; i++) {
        v->choices[i] = vn_str(&strs, vn_choice_text(&m, i));
        v->enabled[i] = vn_choice_enabled(&m, i);
    }
    v->selected = 0;
}

int main(int argc, char **argv)
{
    const char *font_path = argc > 1 ? argv[1] : "demo/assets/font8x16.vnf";
    const char *vnb_path  = argc > 2 ? argv[2] : "build/demo.vnb";
    const char *vnstr_path= argc > 3 ? argv[3] : "build/demo.vnstr";
    const char *out_dir   = argc > 4 ? argv[4] : ".";

    size_t fn = load_file(font_path, g_font_blob, sizeof g_font_blob);
    vn_font font;
    if (!fn || !vn_font_load(&font, g_font_blob, fn)) {
        fprintf(stderr, "bad font %s\n", font_path); return 1;
    }

    size_t cn = load_file(vnb_path, g_code_blob, sizeof g_code_blob);
    size_t sn = load_file(vnstr_path, g_str_blob, sizeof g_str_blob);
    if (!cn || !vn_program_load(&prog, g_code_blob, cn)) {
        fprintf(stderr, "bad bytecode %s\n", vnb_path); return 1;
    }
    if (!sn || !vn_strings_load(&strs, g_str_blob, sn)) {
        fprintf(stderr, "bad strings %s\n", vnstr_path); return 1;
    }

    vn_surface fore;
    vn_surface_init(&fore, g_fore_px, VN_SCREEN_W, VN_SCREEN_H);
    vn_palette pal;
    vn_palette_default(&pal);

    vn_machine_init(&m, &prog, 0);
    vn_profile_init(&prof);

    char path[512];
    uint32_t clock = 0;
    int frame = 0;

    /* A fixed, scripted playthrough so the frames are reproducible and
     * can be hashed as golden output. */
    for (int beat = 0; beat < 40 && frame < 12; beat++) {
        if (vn_run(&m, &prog, &prof, clock, 100000) != VN_RUN_BLOCKED) break;

        demo_view v;
        view_from_stage(&v);

        int is_choice = v.nchoices > 0;
        int is_line = m.current != 0xFF &&
                      m.thread[m.current].state == VN_TH_BLOCKED_SAY;

        if (is_line || is_choice) {
            /* Draw a partly-revealed frame for dialogue, so the golden
             * set covers the typewriter path too. */
            if (is_line) v.reveal = (frame % 3 == 0) ? 18 : -1;
            vn_surface_clear(&fore, 0);
            demo_render(&fore, &font, &v);
            vn_palette_expand(&fore, &pal, g_rgb);
            snprintf(path, sizeof path, "%s/vm_%02d.ppm", out_dir, frame++);
            vn_headless_set_output(path);
            vn_plat_blit(g_rgb, fore.w, fore.h);
        }

        if (is_choice) { vn_choose(&m, 0); continue; }
        if (is_line)   { vn_advance(&m); vn_advance(&m); continue; }
        clock += 100;
    }

    /* Prove save/restore end to end from the real driver, not just the
     * unit tests. */
    static uint8_t save[1 << 16];
    size_t written = 0;
    vn_save_status st = vn_save_write(&m, &prog, &strs, save, sizeof save, &written);
    fprintf(stderr, "demo: %d frames, %llu instructions, save %zu bytes (%s)\n",
            frame, (unsigned long long)m.steps, written, vn_save_status_str(st));
    if (m.error) fprintf(stderr, "demo: VM error: %s\n", m.error_msg);
    return m.error ? 1 : 0;
}
