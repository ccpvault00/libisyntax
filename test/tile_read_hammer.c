// tile_read_hammer SLIDE [THREADS REGIONS SEED]
// N pthreads read a seeded list of 4x4-tile regions (70 % level 0, 30 % random higher level) from ONE isyntax_t
// and ONE cache through libisyntax_tile_read(). Prints one FNV-1a hash per region in list order (so runs with
// different thread counts can be diffed) and the wall time. With no thread count given it runs the list with 1
// thread and then with 8 and exits non-zero if any region hashes differently - the pass/fail test used by CMake
// and meson. Suggested manual use: `tile_read_hammer slide.isyntax 16 1000 7 > /dev/null` for timing.
#include "libisyntax.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
typedef struct { int level, tx, ty; } region_t;
static region_t* regions; static uint64_t* hashes; static int nregions, nthreads;
static isyntax_t* isyntax; static isyntax_cache_t* cache; static int tw, th;
static uint64_t fnv(uint64_t h, const uint32_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; } return h; }
static void* worker(void* arg) {
    int t = (int)(intptr_t)arg; uint32_t* buf = malloc((size_t)tw * th * 4);
    for (int i = t; i < nregions; i += nthreads) {
        uint64_t h = 1469598103934665603ULL;
        for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) {
            if (libisyntax_tile_read(isyntax, cache, regions[i].level, regions[i].tx + dx, regions[i].ty + dy, buf, LIBISYNTAX_PIXEL_FORMAT_RGBA) != LIBISYNTAX_OK) { fprintf(stderr, "tile read failed\n"); }
            h = fnv(h, buf, (size_t)tw * th);
        }
        hashes[i] = h;
    }
    free(buf); return NULL;
}
static int self_test(const char* path);

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: tile_read_hammer SLIDE [THREADS REGIONS SEED]\n"); return 2; }
    libisyntax_init();
    if (argc < 5) return self_test(argv[1]);
    nthreads = atoi(argv[2]); nregions = atoi(argv[3]); srand((unsigned)atoi(argv[4]));
    if (libisyntax_open(argv[1], 0, &isyntax) != LIBISYNTAX_OK) { fprintf(stderr, "open failed\n"); return 1; }
    libisyntax_cache_create("lhammer", 1000, &cache); libisyntax_cache_inject(cache, isyntax);
    tw = libisyntax_get_tile_width(isyntax); th = libisyntax_get_tile_height(isyntax);
    const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax); int levels = libisyntax_image_get_level_count(wsi);
    regions = malloc(sizeof(region_t) * nregions); hashes = calloc(nregions, sizeof(uint64_t));
    for (int i = 0; i < nregions; i++) {
        int lv = (rand() % 10 < 7) ? 0 : 1 + rand() % (levels - 1);
        const isyntax_level_t* L = libisyntax_image_get_level(wsi, lv);
        int wt = libisyntax_level_get_width_in_tiles(L), ht = libisyntax_level_get_height_in_tiles(L);
        regions[i].level = lv; regions[i].tx = wt > 4 ? rand() % (wt - 4) : 0; regions[i].ty = ht > 4 ? rand() % (ht - 4) : 0;
    }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t* thr = malloc(sizeof(pthread_t) * nthreads);
    for (int t = 0; t < nthreads; t++) pthread_create(&thr[t], NULL, worker, (void*)(intptr_t)t);
    for (int t = 0; t < nthreads; t++) pthread_join(thr[t], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    for (int i = 0; i < nregions; i++) printf("%d L%d %d,%d %016llx\n", i, regions[i].level, regions[i].tx, regions[i].ty, (unsigned long long)hashes[i]);
    fprintf(stderr, "TIME threads=%d regions=%d secs=%.2f regions/s=%.2f\n", nthreads, nregions, secs, nregions / secs);
    libisyntax_cache_destroy(cache); libisyntax_close(isyntax); return 0;
}

// 1 thread, then 8 threads, same 200 regions: every hash must match.
static int self_test(const char* path) {
    nregions = 200;
    if (libisyntax_open(path, 0, &isyntax) != LIBISYNTAX_OK) { fprintf(stderr, "open failed\n"); return 1; }
    libisyntax_cache_create("hammer", 1000, &cache); libisyntax_cache_inject(cache, isyntax);
    tw = libisyntax_get_tile_width(isyntax); th = libisyntax_get_tile_height(isyntax);
    const isyntax_image_t* wsi = libisyntax_get_wsi_image(isyntax); int levels = libisyntax_image_get_level_count(wsi);
    regions = malloc(sizeof(region_t) * nregions); hashes = calloc(nregions, sizeof(uint64_t));
    srand(1);
    for (int i = 0; i < nregions; i++) {
        int lv = (rand() % 10 < 7) ? 0 : 1 + rand() % (levels - 1);
        const isyntax_level_t* L = libisyntax_image_get_level(wsi, lv);
        int wt = libisyntax_level_get_width_in_tiles(L), ht = libisyntax_level_get_height_in_tiles(L);
        regions[i].level = lv; regions[i].tx = wt > 4 ? rand() % (wt - 4) : 0; regions[i].ty = ht > 4 ? rand() % (ht - 4) : 0;
    }
    uint64_t* reference = malloc(sizeof(uint64_t) * nregions);
    int bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        nthreads = pass == 0 ? 1 : 8;
        struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
        pthread_t* thr = malloc(sizeof(pthread_t) * nthreads);
        for (int t = 0; t < nthreads; t++) pthread_create(&thr[t], NULL, worker, (void*)(intptr_t)t);
        for (int t = 0; t < nthreads; t++) pthread_join(thr[t], NULL);
        clock_gettime(CLOCK_MONOTONIC, &t1); free(thr);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("%d thread(s): %d regions in %.2f s (%.1f regions/s)\n", nthreads, nregions, secs, nregions / secs);
        if (pass == 0) memcpy(reference, hashes, sizeof(uint64_t) * nregions);
        else for (int i = 0; i < nregions; i++) if (hashes[i] != reference[i]) bad++;
    }
    printf("tile_read_hammer: %d of %d regions differ between 1 and 8 threads -> %s\n", bad, nregions, bad ? "FAILED" : "passed");
    libisyntax_cache_destroy(cache); libisyntax_close(isyntax);
    return bad ? 1 : 0;
}
