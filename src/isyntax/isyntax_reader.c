/*
  BSD 2-Clause License

  Copyright (c) 2019-2025, Pieter Valkema, Alexandr Virodov

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
     list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "common.h"
#include "isyntax_reader.h"

#define LOG(msg, ...) console_print(msg, ##__VA_ARGS__)
#define LOG_VAR(fmt, var) console_print("%s: %s=" fmt "\n", __FUNCTION__, #var, var)

void tile_list_init(isyntax_tile_list_t* list, const char* dbg_name) {
    list->head = NULL;
    list->tail = NULL;
    list->count = 0;
    list->dbg_name = dbg_name;
}

void tile_list_remove(isyntax_tile_list_t* list, isyntax_tile_t* tile) {
    if (!tile->cache_next && !tile->cache_prev && !(list->head == tile) && !(list->tail == tile)) {
        // Not part of any list.
        return;
    }
    if (list->head == tile) {
        list->head = tile->cache_next;
    }
    if (list->tail == tile) {
        list->tail = tile->cache_prev;
    }
    if (tile->cache_prev) {
        tile->cache_prev->cache_next = tile->cache_next;
    }
    if (tile->cache_next) {
        tile->cache_next->cache_prev = tile->cache_prev;
    }
    // Here we assume that the tile is part of this list, but we don't check (O(n)).
    tile->cache_next = NULL;
    tile->cache_prev = NULL;
    list->count--;
}

static void tile_list_insert_first(isyntax_tile_list_t* list, isyntax_tile_t* tile) {
    // printf("### tile_list_insert_first %s scale=%d x=%d y=%d\n", list->dbg_name, tile->tile_scale, tile->tile_x, tile->tile_y);
    ASSERT(tile->cache_next == NULL && tile->cache_prev == NULL);
    if (list->head == NULL) {
        list->head = tile;
        list->tail = tile;
    } else {
        list->head->cache_prev = tile;
        tile->cache_next = list->head;
        list->head = tile;
    }
    list->count++;
}

static void tile_list_insert_list_first(isyntax_tile_list_t* target_list, isyntax_tile_list_t* source_list) {
    if (source_list->head == NULL && source_list->tail == NULL) {
        return;
    }

    source_list->tail->cache_next = target_list->head;
    if (target_list->head) {
        target_list->head->cache_prev = source_list->tail;
    }

    target_list->head = source_list->head;
    if (target_list->tail == NULL) {
        target_list->tail = source_list->tail;
    }
    target_list->count += source_list->count;
    source_list->head = NULL;
    source_list->tail = NULL;
    source_list->count = 0;
}

#define ITERATE_TILE_LIST(_iter, _list) \
    isyntax_tile_t* _iter = _list.head; _iter; _iter = _iter->cache_next


// Returns LIBISYNTAX_OK, LIBISYNTAX_IO_ERROR (a codeblock could not be read: short read / truncated file) or
// LIBISYNTAX_FATAL (a codeblock could not be decoded). The tile is still marked as loaded (with zero coefficients)
// so that the cache stays consistent; the caller is expected to report the error.
static isyntax_error_t isyntax_openslide_load_tile_coefficients_ll_or_h(isyntax_cache_t* cache,
                                                             isyntax_t* isyntax, isyntax_tile_t* tile,
                                                             int codeblock_index, bool is_ll) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_data_chunk_t* chunk = &wsi->data_chunks[tile->data_chunk_index];
    isyntax_error_t result = LIBISYNTAX_OK;

    for (int color = 0; color < 3; ++color) {
        isyntax_codeblock_t* codeblock = &wsi->codeblocks[codeblock_index + color * chunk->codeblock_count_per_color];
        ASSERT(codeblock->coefficient == (is_ll ? 0 : 1)); // LL coefficient codeblock for this tile.
        // TODO(avirodov): int vs i32 vs u32 consistently.
        ASSERT(codeblock->color_component == (u32)color);
        ASSERT(codeblock->scale == (u32)tile->tile_scale);
        if (is_ll) {
            tile->color_channels[color].coeff_ll = (icoeff_t *) block_alloc(cache->ll_coeff_block_allocator);
        } else {
            tile->color_channels[color].coeff_h = (icoeff_t *) block_alloc(cache->h_coeff_block_allocator);
        }
        // TODO(avirodov): fancy allocators, for multiple sequential blocks (aka chunk). Or let OS do the caching.
        // Adding 7 safety bytes so bitstream_lsb_read() won't access out of bounds in isyntax_hulsken_decompress().
        // They are zero-initialised: the 64-bit reads at the end of the block pull them in (valgrind otherwise
        // reports uninitialised values; the decoded symbols never depend on them).
        u8* codeblock_data = calloc(codeblock->block_size + 7, 1);
        icoeff_t* out_buffer = is_ll ? tile->color_channels[color].coeff_ll : tile->color_channels[color].coeff_h;
        size_t bytes_read = file_handle_read_at_offset(codeblock_data, isyntax->file_handle,
                                                       codeblock->block_data_offset, codeblock->block_size);
        if (bytes_read != codeblock->block_size) {
            console_print_error("Error: could not read iSyntax data at offset %lld (read size %lld, got %zu)\n",
                                codeblock->block_data_offset, codeblock->block_size, bytes_read);
            memset(out_buffer, 0, (is_ll ? 1 : 3) * isyntax->block_width * isyntax->block_height * sizeof(icoeff_t));
            result = LIBISYNTAX_IO_ERROR;
        } else if (!isyntax_hulsken_decompress(codeblock_data, codeblock->block_size,
                                               isyntax->block_width, isyntax->block_height,
                                               codeblock->coefficient, wsi->compressor_version, out_buffer)) {
            console_print_error("Error: could not decode iSyntax codeblock at offset %lld (size %lld)\n",
                                codeblock->block_data_offset, codeblock->block_size);
            if (result == LIBISYNTAX_OK) result = LIBISYNTAX_FATAL;
        }
        free(codeblock_data);
    }

    if (is_ll) {
        tile->has_ll = true;
    } else {
        tile->has_h = true;
    }
    return result;
}

// Combines two error codes: an I/O error (truncated file) takes precedence over a decode error.
static isyntax_error_t worse_error(isyntax_error_t a, isyntax_error_t b) {
    if (a == LIBISYNTAX_IO_ERROR || b == LIBISYNTAX_IO_ERROR) return LIBISYNTAX_IO_ERROR;
    return a != LIBISYNTAX_OK ? a : b;
}

static isyntax_error_t isyntax_openslide_load_tile_coefficients(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_error_t result = LIBISYNTAX_OK;

    if (!tile->exists) {
        return LIBISYNTAX_OK;
    }

    // Load LL codeblocks here only for top-level tiles. For other levels, the LL coefficients are computed from parent
    // tiles later on.
    if (tile->tile_scale == wsi->max_scale && !tile->has_ll) {
        result = worse_error(result, isyntax_openslide_load_tile_coefficients_ll_or_h(
                cache, isyntax, tile, /*codeblock_index=*/tile->codeblock_index, /*is_ll=*/true));
    }

    if (!tile->has_h) {
        ASSERT(tile->exists);
        isyntax_data_chunk_t* chunk = wsi->data_chunks + tile->data_chunk_index;

        i32 scale_in_chunk = chunk->scale - tile->tile_scale;
        ASSERT(scale_in_chunk >= 0 && scale_in_chunk < 3);
        i32 codeblock_index_in_chunk = 0;
        if (scale_in_chunk == 0) {
            codeblock_index_in_chunk = 0;
        } else if (scale_in_chunk == 1) {
            codeblock_index_in_chunk = 1 + (tile->tile_y % 2) * 2 + (tile->tile_x % 2);
        } else if (scale_in_chunk == 2) {
            codeblock_index_in_chunk = 5 + (tile->tile_y % 4) * 4 + (tile->tile_x % 4);
        } else {
            fatal_error();
        }

        result = worse_error(result, isyntax_openslide_load_tile_coefficients_ll_or_h(
                cache, isyntax, tile,
                /*codeblock_index=*/tile->codeblock_chunk_index + codeblock_index_in_chunk, /*is_ll=*/false));
    }
    return result;
}

typedef union isyntax_tile_children_t {
    struct {
        isyntax_tile_t *child_top_left;
        isyntax_tile_t *child_top_right;
        isyntax_tile_t *child_bottom_left;
        isyntax_tile_t *child_bottom_right;
    };
    isyntax_tile_t* as_array[4];
} isyntax_tile_children_t;

static isyntax_tile_children_t isyntax_openslide_compute_children(isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_tile_children_t result;
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    ASSERT(tile->tile_scale > 0);
    isyntax_level_t *next_level = &wsi->levels[tile->tile_scale - 1];
    result.child_top_left = isyntax_level_tiles(wsi, tile->tile_scale - 1) + (tile->tile_y * 2) * next_level->width_in_tiles + (tile->tile_x * 2);
    result.child_top_right = result.child_top_left + 1;
    result.child_bottom_left = result.child_top_left + next_level->width_in_tiles;
    result.child_bottom_right = result.child_bottom_left + 1;
    return result;
}


static void isyntax_openslide_idwt(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile,
                                   uint32_t* pixels_buffer, enum isyntax_pixel_format_t pixel_format) {
    if (tile->tile_scale == 0) {
        ASSERT(pixels_buffer != NULL); // Shouldn't be asking for idwt at level 0 if we're not going to use the result for pixels.
        isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                                 tile->tile_scale, tile->tile_x, tile->tile_y,
                                 cache->ll_coeff_block_allocator,
                                 pixels_buffer, pixel_format);
        return;
    }

    if (pixels_buffer != NULL) {
        // TODO(avirodov): if we want rgb from tile where idwt was done already, this could be cheaper if we store
        //  the lls in the tile. Currently need to recompute idwt.
        isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                          tile->tile_scale, tile->tile_x, tile->tile_y,
                          cache->ll_coeff_block_allocator,
                          pixels_buffer, pixel_format);
        return;
    }

    // If all children have ll coefficients and we don't need the rgb pixels, no need to do the idwt.
    ASSERT(pixels_buffer == NULL && tile->tile_scale > 0);
    isyntax_tile_children_t children = isyntax_openslide_compute_children(isyntax, tile);
    if (children.child_top_left->has_ll && children.child_top_right->has_ll &&
        children.child_bottom_left->has_ll && children.child_bottom_right->has_ll) {
        return;
    }

    isyntax_load_tile(isyntax, &isyntax->images[isyntax->wsi_image_index],
                      tile->tile_scale, tile->tile_x, tile->tile_y,
                      cache->ll_coeff_block_allocator,
                      /*pixels_buffer=*/NULL, /*pixel_format=*/0);
}


// ---------------------------------------------------------------------------------------------------------------------
// Concurrent tile reads on one cache.
//
// A read needs a working set of tiles: the requested tile and its ancestors (idwt_set, inverse wavelet transforms that
// produce LL coefficients for the next level), their neighbours (coeff_set, their H coefficients feed the IDWT margins)
// and the children of every IDWT tile (children_set, they receive LL coefficients). The set is computed with the cache
// mutex held and every tile in it is reserved (reader_refcount), which keeps it out of the LRU list and thus away from
// eviction. The expensive work (codeblock I/O + Hulsken decode, IDWT, colour conversion) then runs outside the mutex,
// serialised per tile by reader_in_progress: a thread that needs a tile another thread is working on waits on the
// condition variable instead of duplicating or racing the work. A thread never waits while it holds reader_in_progress
// on a tile, so there are no wait cycles. Results are deterministic because an IDWT only runs once all of its
// dependencies are loaded, and LL coefficients that exist are never rewritten (see isyntax_load_tile()).
// ---------------------------------------------------------------------------------------------------------------------

typedef struct tile_array_t {
    isyntax_tile_t** items;
    int count;
    int capacity;
} tile_array_t;

static void tile_array_push(tile_array_t* array, isyntax_tile_t* tile) {
    if (array->count == array->capacity) {
        array->capacity = array->capacity ? array->capacity * 2 : 64;
        array->items = realloc(array->items, array->capacity * sizeof(*array->items));
    }
    array->items[array->count++] = tile;
}

// Mutex held. Marks the tile as visited for this working-set computation and reserves it.
static void reserve_tile(isyntax_cache_t* cache, isyntax_tile_t* tile, tile_array_t* set) {
    tile->cache_marked = true;
    if (tile->reader_refcount++ == 0) {
        tile_list_remove(&cache->cache_list, tile); // no-op if it was never cached
    }
    tile_array_push(set, tile);
}

// Mutex held.
static void reserve_parent(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile, tile_array_t* idwt_set) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    int parent_tile_scale = tile->tile_scale + 1;
    if (parent_tile_scale > wsi->max_scale) {
        return;
    }
    isyntax_level_t* parent_level = &wsi->levels[parent_tile_scale];
    isyntax_tile_t* parent_tile = &isyntax_level_tiles(wsi, parent_tile_scale)[parent_level->width_in_tiles * (tile->tile_y / 2) + (tile->tile_x / 2)];
    if (parent_tile->exists && !parent_tile->cache_marked) {
        reserve_tile(cache, parent_tile, idwt_set);
    }
}

// Mutex held. Same traversal as the original single-threaded list builder: per scale, the neighbours of the IDWT tiles
// need coefficients, and the parents of both groups need an IDWT (which gives this scale its LL coefficients).
// The arrays are in insertion order, i.e. idwt_set[0] is the requested tile and higher indices are higher scales.
static void build_working_set(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* requested,
                              tile_array_t* idwt_set, tile_array_t* coeff_set, tile_array_t* children_set) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    reserve_tile(cache, requested, idwt_set);
    for (int scale = requested->tile_scale; scale <= wsi->max_scale; ++scale) {
        isyntax_level_t* level = &wsi->levels[scale];
        isyntax_tile_t* level_tiles = isyntax_level_tiles(wsi, scale);
        for (int i = 0; i < idwt_set->count; ++i) {
            isyntax_tile_t* tile = idwt_set->items[i];
            if (tile->tile_scale != scale) {
                continue;
            }
            for (int y_offset = -1; y_offset <= 1; ++y_offset) {
                for (int x_offset = -1; x_offset <= 1; ++x_offset) {
                    int neighbor_tile_x = tile->tile_x + x_offset;
                    int neighbor_tile_y = tile->tile_y + y_offset;
                    if (neighbor_tile_x < 0 || neighbor_tile_x >= level->width_in_tiles ||
                        neighbor_tile_y < 0 || neighbor_tile_y >= level->height_in_tiles) {
                        continue;
                    }
                    isyntax_tile_t* neighbor_tile = &level_tiles[level->width_in_tiles * neighbor_tile_y + neighbor_tile_x];
                    if (neighbor_tile->cache_marked || !neighbor_tile->exists) {
                        continue;
                    }
                    reserve_tile(cache, neighbor_tile, coeff_set);
                }
            }
        }
        for (int i = 0; i < idwt_set->count; ++i) {
            if (idwt_set->items[i]->tile_scale == scale) {
                reserve_parent(cache, isyntax, idwt_set->items[i], idwt_set);
            }
        }
        for (int i = 0; i < coeff_set->count; ++i) {
            if (coeff_set->items[i]->tile_scale == scale) {
                reserve_parent(cache, isyntax, coeff_set->items[i], idwt_set);
            }
        }
    }
    // Children of IDWT tiles receive LL coefficients; reserve them so that they are not evicted (or re-decoded by
    // another thread) while being written.
    for (int i = 0; i < idwt_set->count; ++i) {
        isyntax_tile_t* tile = idwt_set->items[i];
        if (tile->tile_scale > 0) {
            isyntax_tile_children_t children = isyntax_openslide_compute_children(isyntax, tile);
            for (int c = 0; c < 4; ++c) {
                if (!children.as_array[c]->cache_marked) {
                    reserve_tile(cache, children.as_array[c], children_set);
                }
            }
        }
    }
    // The visit marks are shared between threads and only valid inside this critical section.
    for (int i = 0; i < idwt_set->count; ++i)     { idwt_set->items[i]->cache_marked = false; }
    for (int i = 0; i < coeff_set->count; ++i)    { coeff_set->items[i]->cache_marked = false; }
    for (int i = 0; i < children_set->count; ++i) { children_set->items[i]->cache_marked = false; }
}

// Mutex held. Blocks until no other thread works on the tile.
static void wait_for_tile(isyntax_cache_t* cache, isyntax_tile_t* tile) {
    while (tile->reader_in_progress) {
        platform_condition_variable_wait(&cache->cond, &cache->mutex);
    }
}

// Mutex held. Loads the H (and top-level LL) coefficients of a tile unless they are already present. Returns the
// error code if the tile's codeblocks could not be read or decoded (by this thread or earlier by another one).
static isyntax_error_t ensure_tile_coefficients(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    if (!tile->exists) {
        return LIBISYNTAX_OK;
    }
    wait_for_tile(cache, tile);
    bool needs_h = !tile->has_h;
    // Test the scale first: has_ll of a lower tile may be written by its parent's IDWT on another thread right now.
    bool needs_ll = tile->tile_scale == wsi->max_scale && !tile->has_ll;
    if (needs_h || needs_ll) {
        tile->reader_in_progress = true;
        platform_mutex_unlock(&cache->mutex);
        isyntax_error_t error = isyntax_openslide_load_tile_coefficients(cache, isyntax, tile);
        platform_mutex_lock(&cache->mutex);
        if (error != LIBISYNTAX_OK) {
            tile->reader_error = (int8_t)error;
        }
        tile->reader_in_progress = false;
        platform_condition_variable_broadcast(&cache->cond);
    }
    return (isyntax_error_t)tile->reader_error;
}

// Mutex held. Runs the IDWT of a tile (writing its children's LL coefficients, and the pixels if requested) unless
// its children already have their LL coefficients and no pixels are needed.
static void ensure_tile_idwt(isyntax_cache_t* cache, isyntax_t* isyntax, isyntax_tile_t* tile,
                             uint32_t* pixels_buffer, enum isyntax_pixel_format_t pixel_format) {
    wait_for_tile(cache, tile);
    if (pixels_buffer == NULL) {
        isyntax_tile_children_t children = isyntax_openslide_compute_children(isyntax, tile);
        if (children.child_top_left->has_ll && children.child_top_right->has_ll &&
            children.child_bottom_left->has_ll && children.child_bottom_right->has_ll) {
            return;
        }
    }
    tile->reader_in_progress = true;
    platform_mutex_unlock(&cache->mutex);
    isyntax_openslide_idwt(cache, isyntax, tile, pixels_buffer, pixel_format);
    platform_mutex_lock(&cache->mutex);
    tile->reader_in_progress = false;
    platform_condition_variable_broadcast(&cache->cond);
}

// Mutex held. Drops this thread's reservations; tiles nobody else uses go to the front of the LRU list.
static void release_set(isyntax_cache_t* cache, tile_array_t* set) {
    for (int i = 0; i < set->count; ++i) {
        isyntax_tile_t* tile = set->items[i];
        if (--tile->reader_refcount == 0) {
            tile_list_insert_first(&cache->cache_list, tile);
        }
    }
    free(set->items);
}

isyntax_error_t isyntax_tile_read(isyntax_t* isyntax, isyntax_cache_t* cache, int scale, int tile_x, int tile_y,
                                  uint32_t* pixels_buffer, enum isyntax_pixel_format_t pixel_format) {
    isyntax_image_t* wsi = &isyntax->images[isyntax->wsi_image_index];
    isyntax_level_t* level = &wsi->levels[scale];

    if (!(tile_x >= 0 && tile_x < level->width_in_tiles && tile_y >= 0 && tile_y < level->height_in_tiles)) {
        // Read out of bounds -> set to all white
        memset(pixels_buffer, 0xff, isyntax->tile_width * isyntax->tile_height * 4);
        return LIBISYNTAX_OK;
    }
    // tile_entries is immutable after open, so the existence check needs no lock; the tile table itself is
    // built (once) under the mutex.
    if (!level->tile_entries[level->width_in_tiles * tile_y + tile_x].exists) {
        memset(pixels_buffer, 0xff, isyntax->tile_width * isyntax->tile_height * 4);
        return LIBISYNTAX_OK;
    }

    tile_array_t idwt_set = {0};
    tile_array_t coeff_set = {0};
    tile_array_t children_set = {0};
    isyntax_error_t result = LIBISYNTAX_OK;

    platform_mutex_lock(&cache->mutex);
    // Materialise every level this read can touch (children of the requested tile up to the top level) here, so
    // that the accessor calls below and the lock-free uses inside isyntax_load_tile() never see a NULL table.
    for (int s = (scale > 0 ? scale - 1 : 0); s <= wsi->max_scale; ++s) {
        if (!isyntax_level_tiles(wsi, s)) {
            platform_mutex_unlock(&cache->mutex);
            return LIBISYNTAX_FATAL;
        }
    }
    isyntax_tile_t* tile = &level->tiles[level->width_in_tiles * tile_y + tile_x];
    build_working_set(cache, isyntax, tile, &idwt_set, &coeff_set, &children_set);

    // Coefficients for every tile that feeds an IDWT.
    for (int i = 0; i < coeff_set.count; ++i) {
        result = worse_error(result, ensure_tile_coefficients(cache, isyntax, coeff_set.items[i]));
    }
    for (int i = 0; i < idwt_set.count; ++i) {
        result = worse_error(result, ensure_tile_coefficients(cache, isyntax, idwt_set.items[i]));
    }

    // IDWT from the top level down: each one provides the LL coefficients of the next level. The requested tile
    // (idwt_set[0]) is last and also produces the pixels.
    for (int i = idwt_set.count - 1; i >= 0; --i) {
        bool is_requested = (i == 0);
        ensure_tile_idwt(cache, isyntax, idwt_set.items[i],
                         is_requested ? pixels_buffer : NULL, is_requested ? pixel_format : 0);
    }

    // Bump all the affected tiles in the cache (same LRU order as before: IDWT tiles first, then neighbours, then
    // children), then trim. Tiles still reserved by other threads are left out of the list until they release them.
    release_set(cache, &children_set);
    release_set(cache, &coeff_set);
    release_set(cache, &idwt_set);

    while (cache->cache_list.count > cache->target_cache_size) {
        isyntax_tile_t* evicted = cache->cache_list.tail;
        tile_list_remove(&cache->cache_list, evicted);
        for (int i = 0; i < 3; ++i) {
            if (evicted->has_ll) {
                block_free(cache->ll_coeff_block_allocator, evicted->color_channels[i].coeff_ll);
                evicted->color_channels[i].coeff_ll = NULL;
            }
            if (evicted->has_h) {
                block_free(cache->h_coeff_block_allocator, evicted->color_channels[i].coeff_h);
                evicted->color_channels[i].coeff_h = NULL;
            }
        }
        evicted->has_ll = false;
        evicted->has_h = false;
        evicted->reader_error = 0;
    }

    // Prevent iSyntax streamer from calling isyntax_begin_first_load()
    if (!wsi->first_load_complete) {
        wsi->first_load_complete = true;
    }
    platform_mutex_unlock(&cache->mutex);
    return result;
}
