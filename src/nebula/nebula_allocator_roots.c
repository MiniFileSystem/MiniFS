/*
 * nebula_allocator_roots.c - Initialize 128 allocator roots split 64+64.
 */
#include "nebula_allocator_roots.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

uint32_t nebula_allocator_root_checksum(const struct nebula_allocator_root *r)
{
    struct nebula_allocator_root tmp = *r;
    tmp.checksum = 0;
    return crc32c(&tmp, sizeof(tmp));
}

static void fill_roots(struct nebula_allocator_root *roots,
                       uint32_t start_id, uint32_t count,
                       uint64_t data_start, uint64_t data_end,
                       uint32_t total_roots,
                       uint64_t bitmap_lba, uint64_t stream_map_lba)
{
    uint64_t total_blocks = data_end - data_start;
    uint64_t per_root     = total_blocks / total_roots;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t id = start_id + i;
        struct nebula_allocator_root *r = &roots[i];
        memset(r, 0, sizeof(*r));
        r->root_id             = id;
        r->range_start_block   = data_start + (uint64_t)id * per_root;
        /* Last root swallows the remainder. */
        if (id == total_roots - 1) {
            r->range_end_block = data_end;
        } else {
            r->range_end_block = data_start + (uint64_t)(id + 1) * per_root;
        }
        r->free_block_count    = r->range_end_block - r->range_start_block;
        r->last_alloc_hint     = r->range_start_block;
        r->bitmap_region_lba   = bitmap_lba;
        r->stream_map_region_lba = stream_map_lba;
        r->checksum            = nebula_allocator_root_checksum(r);
    }
}

int nebula_allocator_roots_init(struct nebula_io *io,
                                const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    void *block = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!block) return -ENOMEM;
    memset(block, 0, NEBULA_BLOCK_SIZE);
    struct nebula_allocator_root *roots = block;

    /* Head copy: root_ids 0..63 */
    fill_roots(roots, 0, NEBULA_ALLOC_ROOTS_HEAD,
               L->data_start_lba, L->alloc_roots_tail_lba,
               NEBULA_ALLOC_ROOTS_TOTAL,
               L->bitmap_lba, L->stream_map_lba);
    int rc = nebula_io_write(io, L->alloc_roots_head_lba, 1, block);
    if (rc != NEBULA_OK) { free(block); return rc; }

    /* Tail copy: root_ids 64..127 */
    memset(block, 0, NEBULA_BLOCK_SIZE);
    fill_roots(roots, NEBULA_ALLOC_ROOTS_HEAD, NEBULA_ALLOC_ROOTS_TAIL,
               L->data_start_lba, L->alloc_roots_tail_lba,
               NEBULA_ALLOC_ROOTS_TOTAL,
               L->bitmap_lba, L->stream_map_lba);
    rc = nebula_io_write(io, L->alloc_roots_tail_lba, 1, block);
    free(block);
    return rc;
}

int nebula_allocator_roots_read(struct nebula_io *io, nebula_lba_t lba,
                                struct nebula_allocator_root roots[NEBULA_ALLOC_ROOTS_HEAD])
{
    if (!io || !roots) return -EINVAL;

    void *block = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!block) return -ENOMEM;

    int rc = nebula_io_read(io, lba, 1, block);
    if (rc != NEBULA_OK) { free(block); return rc; }

    memcpy(roots, block, sizeof(struct nebula_allocator_root) * NEBULA_ALLOC_ROOTS_HEAD);
    free(block);
    return NEBULA_OK;
}
