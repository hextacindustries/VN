/* Headless platform port: renders to PPM files instead of a window.
 *
 * This exists so the core can be built and verified with ZERO external
 * dependencies. It is also what CI uses for golden-frame tests, and it
 * is the reference every other port is checked against - if the SDL,
 * WebAssembly or watchOS shells disagree with these bytes, the shell is
 * wrong, not the core.
 */
#include "vn_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char        g_out_path[512] = "frame.ppm";
static vn_input    g_queue[64];
static int         g_queue_head = 0, g_queue_tail = 0;

void vn_headless_set_output(const char *path)
{
    snprintf(g_out_path, sizeof g_out_path, "%s", path);
}

void vn_headless_push_input(vn_input in)
{
    int next = (g_queue_tail + 1) % (int)(sizeof g_queue / sizeof g_queue[0]);
    if (next == g_queue_head) return;      /* full: drop, never block */
    g_queue[g_queue_tail] = in;
    g_queue_tail = next;
}

void vn_plat_blit(const uint32_t *pixels, int w, int h)
{
    FILE *f = fopen(g_out_path, "wb");
    if (!f) { vn_plat_log("headless: cannot open output"); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (long i = 0, n = (long)w * h; i < n; i++) {
        uint32_t p = pixels[i];
        unsigned char rgb[3] = {
            (unsigned char)((p >> 16) & 0xFF),
            (unsigned char)((p >> 8) & 0xFF),
            (unsigned char)(p & 0xFF)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int vn_plat_scale(void) { return 1; }

void vn_plat_audio_push(const int16_t *samples, int frames)
{
    (void)samples; (void)frames;   /* silently discarded */
}

uint32_t vn_plat_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

vn_input vn_plat_poll_input(void)
{
    if (g_queue_head == g_queue_tail) return VN_INPUT_NONE;
    vn_input in = g_queue[g_queue_head];
    g_queue_head = (g_queue_head + 1) % (int)(sizeof g_queue / sizeof g_queue[0]);
    return in;
}

void *vn_plat_file_open(const char *path) { return fopen(path, "rb"); }

size_t vn_plat_file_size(void *handle)
{
    FILE *f = (FILE *)handle;
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, cur, SEEK_SET);
    return (size_t)end;
}

size_t vn_plat_file_read(void *handle, void *dst, size_t n)
{
    return fread(dst, 1, n, (FILE *)handle);
}

void vn_plat_file_close(void *handle) { if (handle) fclose((FILE *)handle); }

int vn_plat_save_write(const char *slot, const void *data, size_t n)
{
    FILE *f = fopen(slot, "wb");
    if (!f) return 0;
    size_t w = fwrite(data, 1, n, f);
    fclose(f);
    return w == n;
}

size_t vn_plat_save_read(const char *slot, void *dst, size_t cap)
{
    FILE *f = fopen(slot, "rb");
    if (!f) return 0;
    size_t r = fread(dst, 1, cap, f);
    fclose(f);
    return r;
}

void vn_plat_log(const char *msg) { fprintf(stderr, "[vn] %s\n", msg); }
void vn_plat_exit(int code) { exit(code); }
