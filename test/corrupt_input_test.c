// corrupt_input_test SLIDE [TMPDIR]
// Makes damaged copies of SLIDE (truncated in the XML header, truncated after ~60 %, random bit flips in the
// second half, a zeroed 16 MB window) and opens + reads tiles from each through the public API. Every case must
// come back with a return code - never abort, crash or hang. Exit status 0 if so. Needs ~2x the slide's size in
// TMPDIR (default: the current directory); the copies are deleted afterwards.
#include "libisyntax.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

static long copy_file(const char* src, const char* dst, long max_bytes) {
    FILE* in = fopen(src, "rb"); FILE* out = fopen(dst, "wb");
    if (!in || !out) { fprintf(stderr, "cannot copy %s -> %s\n", src, dst); exit(2); }
    static char buf[1 << 20]; long total = 0; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (max_bytes >= 0 && total + (long)n > max_bytes) n = (size_t)(max_bytes - total);
        if (n == 0) break;
        fwrite(buf, 1, n, out); total += (long)n;
    }
    fclose(in); fclose(out); return total;
}

static void damage(const char* path, long from, long to, int flips, int zero_window_mb) {
    FILE* f = fopen(path, "r+b"); if (!f) exit(2);
    unsigned seed = 12345;
    for (int i = 0; i < flips; i++) {
        seed = seed * 1103515245u + 12345u; long off = from + (long)(seed % (unsigned)(to - from));
        fseek(f, off, SEEK_SET); int c = fgetc(f); if (c == EOF) continue;
        fseek(f, off, SEEK_SET); fputc(c ^ (1 << (i % 8)), f);
    }
    if (zero_window_mb > 0) {
        static char zeros[1 << 20]; memset(zeros, 0, sizeof(zeros));
        fseek(f, from, SEEK_SET);
        for (int i = 0; i < zero_window_mb; i++) fwrite(zeros, 1, sizeof(zeros), f);
    }
    fclose(f);
}

static void expect(const char* what, const char* path) {
    isyntax_t* s = NULL;
    isyntax_error_t e = libisyntax_open(path, 0, &s);
    printf("%-28s open -> %d", what, e);
    if (e != LIBISYNTAX_OK) { printf(" (error reported, ok)\n"); return; }
    isyntax_cache_t* c; libisyntax_cache_create("corrupt_input_test", 500, &c); libisyntax_cache_inject(c, s);
    const isyntax_image_t* wsi = libisyntax_get_wsi_image(s); int levels = libisyntax_image_get_level_count(wsi);
    int tw = libisyntax_get_tile_width(s), th = libisyntax_get_tile_height(s);
    uint32_t* buf = malloc((size_t)tw * th * 4);
    int errors = 0, reads = 0;
    for (int lv = 0; lv < levels; lv += (levels > 1 ? levels - 1 : 1)) { // level 0 and the top level
        const isyntax_level_t* L = libisyntax_image_get_level(wsi, lv);
        int w = libisyntax_level_get_width_in_tiles(L), h = libisyntax_level_get_height_in_tiles(L);
        for (int y = 0; y < h; y += (h > 8 ? h / 8 : 1)) for (int x = 0; x < w; x += (w > 8 ? w / 8 : 1)) {
            reads++; if (libisyntax_tile_read(s, c, lv, x, y, buf, LIBISYNTAX_PIXEL_FORMAT_RGBA) != LIBISYNTAX_OK) errors++;
        }
        if (levels == 1) break;
    }
    printf("; %d tile reads, %d returned an error (ok either way, the process survived)\n", reads, errors);
    free(buf); libisyntax_cache_destroy(c); libisyntax_close(s);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: corrupt_input_test SLIDE [TMPDIR]\n"); return 2; }
    const char* tmp = argc > 2 ? argv[2] : ".";
    libisyntax_init();
    char p1[4096], p2[4096], p3[4096], p4[4096];
    snprintf(p1, sizeof(p1), "%s/corrupt_trunc_header.isyntax", tmp);
    snprintf(p2, sizeof(p2), "%s/corrupt_trunc_60pct.isyntax", tmp);
    snprintf(p3, sizeof(p3), "%s/corrupt_bitflips.isyntax", tmp);
    snprintf(p4, sizeof(p4), "%s/corrupt_zero_window.isyntax", tmp);
    FILE* f = fopen(argv[1], "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fclose(f);
    copy_file(argv[1], p1, 1 << 20);                 // cut inside the XML header
    copy_file(argv[1], p2, size * 6 / 10);           // cut inside the codeblocks
    copy_file(argv[1], p3, -1); damage(p3, size / 2, size, 2000, 0);        // bit flips in codeblocks
    copy_file(argv[1], p4, -1); damage(p4, size * 4 / 10, size, 0, 16);     // 16 MB of zeros
    expect("intact", argv[1]);
    expect("truncated header", p1);
    expect("truncated at 60%", p2);
    expect("2000 bit flips", p3);
    expect("16 MB zero window", p4);
    remove(p1); remove(p2); remove(p3); remove(p4);
    printf("corrupt_input_test: %s\n", failures ? "FAILED" : "passed (no abort, no hang)");
    return failures ? 1 : 0;
}
