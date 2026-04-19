/*
 * nebula_bitmap.c - Initialize bitmap (with metadata pre-allocation) and
 *                   zero the stream map region.
 *
 * Metadata pre-allocation (closes design-doc gap):
 *   LBAs [0 .. data_start_lba)        => allocated (1)
 *   LBAs [data_start_lba .. alloc_roots_tail_lba) => free (0)
 *   LBAs [alloc_roots_tail_lba .. capacity) => allocated (1)
 *
 * This prevents the allocator from handing out metadata blocks and makes
 * the hierarchical bitmap report only the data region as free on mount.
 */
#include "nebula_bitmap.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int write_zeros(struct nebula_io *io, nebula_lba_t lba, uint64_t n_blocks)
{
    /* Use a batch buffer for speed. */
    enum { BATCH_BLOCKS = 64 };  /* 256 KB */
    const size_t batch_bytes = BATCH_BLOCKS * NEBULA_BLOCK_SIZE;
    void *zbuf = aligned_alloc(NEBULA_BLOCK_SIZE, batch_bytes);
    if (!zbuf) return -ENOMEM;
    memset(zbuf, 0, batch_bytes);

    uint64_t remaining = n_blocks;
    nebula_lba_t cur = lba;
    while (remaining > 0) {
        uint32_t chunk = remaining > BATCH_BLOCKS
                         ? (uint32_t)BATCH_BLOCKS
                         : (uint32_t)remaining;
        int rc = nebula_io_write(io, cur, chunk, zbuf);
        if (rc != NEBULA_OK) { free(zbuf); return rc; }
        cur       += chunk;
        remaining -= chunk;
    }
    free(zbuf);
    return NEBULA_OK;
}

static inline bool lba_is_metadata(const struct nebula_layout *L, uint64_t lba)
{
    return (lba < L->data_start_lba) || (lba >= L->alloc_roots_tail_lba);
}

int nebula_bitmap_init(struct nebula_io *io, const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    uint8_t *page = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!page) return -ENOMEM;

    const uint64_t bits_per_page = (uint64_t)NEBULA_BLOCK_SIZE * 8ULL; /* 32768 */
    const uint64_t cap           = L->capacity_blocks;
    uint64_t       allocated_cnt = 0;

    for (uint64_t p = 0; p < L->bitmap_block_count; p++) {
        memset(page, 0, NEBULA_BLOCK_SIZE);
        uint64_t first_lba = p * bits_per_page;
        uint64_t last_lba  = first_lba + bits_per_page;    /* exclusive */
        if (last_lba > cap) last_lba = cap;

        for (uint64_t lba = first_lba; lba < last_lba; lba++) {
            if (lba_is_metadata(L, lba)) {
                uint64_t bit = lba - first_lba;
                page[bit >> 3] |= (uint8_t)(1U << (bit & 7U));
                allocated_cnt++;
            }
        }
        int rc = nebula_io_write(io, L->bitmap_lba + p, 1, page);
        if (rc != NEBULA_OK) { free(page); return rc; }
    }
    free(page);

    NEB_INFO("Bitmap: pre-marked %lu metadata block(s) as allocated; "
             "%lu data block(s) free",
             (unsigned long)allocated_cnt,
             (unsigned long)(cap - allocated_cnt));

    /* Stream map region starts empty. */
    return write_zeros(io, L->stream_map_lba, L->stream_map_block_count);
}
