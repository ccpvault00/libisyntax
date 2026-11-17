// open_memory_report SLIDE: resident memory of this process after libisyntax_open() + cache_inject(), after reading
// the whole top level, and after one 4x4-tile region of level 0 (Linux: /proc/self/statm). A report, not a pass/fail
// test; the numbers in the commit message come from it.
#include "libisyntax.h"
#include <stdio.h>
#include <stdlib.h>
static long rss_kb(void) { FILE* f = fopen("/proc/self/statm", "r"); long a, b; if (fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; fclose(f); return b * 4; }
int main(int argc, char** argv) {
    libisyntax_init(); long r0 = rss_kb();
    isyntax_t* s; if (libisyntax_open(argv[1], 0, &s) != LIBISYNTAX_OK) return 1;
    isyntax_cache_t* c; libisyntax_cache_create("m", 2000, &c); libisyntax_cache_inject(c, s);
    long r1 = rss_kb();
    const isyntax_image_t* w = libisyntax_get_wsi_image(s); int levels = libisyntax_image_get_level_count(w);
    int tw = libisyntax_get_tile_width(s), th = libisyntax_get_tile_height(s); uint32_t* buf = malloc((size_t)tw * th * 4);
    const isyntax_level_t* top = libisyntax_image_get_level(w, levels - 1);
    for (int y = 0; y < libisyntax_level_get_height_in_tiles(top); y++) for (int x = 0; x < libisyntax_level_get_width_in_tiles(top); x++) libisyntax_tile_read(s, c, levels - 1, x, y, buf, LIBISYNTAX_PIXEL_FORMAT_RGBA);
    long r2 = rss_kb();
    const isyntax_level_t* L0 = libisyntax_image_get_level(w, 0); int cx = libisyntax_level_get_width_in_tiles(L0) / 2, cy = libisyntax_level_get_height_in_tiles(L0) / 2;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) libisyntax_tile_read(s, c, 0, cx + x, cy + y, buf, LIBISYNTAX_PIXEL_FORMAT_RGBA);
    long r3 = rss_kb();
    printf("%s: open+inject +%ld KB; +top level read +%ld KB; +one L0 4x4 region +%ld KB (total %ld KB)\n", argv[1], r1 - r0, r2 - r1, r3 - r2, r3 - r0);
    return 0;
}
