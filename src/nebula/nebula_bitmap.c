/*
 * nebula_bitmap.c - Zero-initialize bitmap and stream map regions.
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

int nebula_bitmap_init(struct nebula_io *io, const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    int rc = write_zeros(io, L->bitmap_lba, L->bitmap_block_count);
    if (rc != NEBULA_OK) return rc;

    rc = write_zeros(io, L->stream_map_lba, L->stream_map_block_count);
    return rc;
}
