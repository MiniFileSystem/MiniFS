/*
 * nebula_uberblock.c - Uberblock region init.
 */
#include "nebula_uberblock.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t nebula_uberblock_checksum(const struct nebula_uberblock *ub)
{
    struct nebula_uberblock tmp = *ub;
    tmp.checksum = 0;
    return crc32c(&tmp, sizeof(tmp));
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int nebula_uberblock_init_region(struct nebula_io *io,
                                 const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    /* Zero-fill whole region (128 slots x 4 KB = 512 KB). */
    void *zero = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!zero) return -ENOMEM;
    memset(zero, 0, NEBULA_BLOCK_SIZE);
    for (uint64_t i = 0; i < L->uberblock_count; i++) {
        int rc = nebula_io_write(io, L->uberblock_lba + i, 1, zero);
        if (rc != NEBULA_OK) { free(zero); return rc; }
    }
    free(zero);

    /* Populate slot 0. */
    struct nebula_uberblock *ub = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!ub) return -ENOMEM;
    memset(ub, 0, NEBULA_BLOCK_SIZE);

    ub->magic                  = NEBULA_MAGIC_UB;
    ub->txg_id                 = 0;
    ub->timestamp_ns           = now_ns();
    ub->bitmap_root_lba        = L->bitmap_lba;
    ub->stream_map_lba         = L->stream_map_lba;
    ub->stream_map_head_offset = 0;
    ub->alloc_roots_count      = NEBULA_ALLOC_ROOTS_TOTAL;
    ub->checksum               = nebula_uberblock_checksum(ub);

    int rc = nebula_io_write(io, L->uberblock_lba, 1, ub);
    free(ub);
    return rc;
}

int nebula_uberblock_read_slot(struct nebula_io *io, const struct nebula_layout *L,
                               uint32_t slot, struct nebula_uberblock *out)
{
    if (!io || !L || !out) return -EINVAL;
    if (slot >= L->uberblock_count) return -ERANGE;

    struct nebula_uberblock *buf = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    int rc = nebula_io_read(io, L->uberblock_lba + slot, 1, buf);
    if (rc != NEBULA_OK) { free(buf); return rc; }

    if (buf->magic != NEBULA_MAGIC_UB) { free(buf); return -ENOENT; }
    uint32_t want = buf->checksum;
    uint32_t got  = nebula_uberblock_checksum(buf);
    if (want != got) { free(buf); return -EIO; }

    *out = *buf;
    free(buf);
    return NEBULA_OK;
}
