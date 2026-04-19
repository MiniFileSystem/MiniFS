/*
 * nebula_hier_bitmap.c - Hierarchical bitmap loader.
 */
#include "nebula_hier_bitmap.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_format.h"
#include "../util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Popcount a byte. */
static inline uint32_t popcount8(uint8_t b)
{
    /* __builtin_popcount would also work; keep explicit for portability. */
    b = (b & 0x55) + ((b >> 1) & 0x55);
    b = (b & 0x33) + ((b >> 2) & 0x33);
    b = (b & 0x0F) + ((b >> 4) & 0x0F);
    return b;
}

int nebula_hbm_load(struct nebula_io *io,
                    nebula_lba_t bitmap_lba,
                    uint64_t bitmap_block_count,
                    uint64_t total_blocks,
                    struct nebula_hier_bitmap **out)
{
    if (!io || !out || bitmap_block_count == 0) return -EINVAL;

    struct nebula_hier_bitmap *h = calloc(1, sizeof(*h));
    if (!h) return -ENOMEM;

    h->total_blocks = total_blocks;
    h->num_pages    = bitmap_block_count;
    h->num_groups   = (uint32_t)((h->num_pages + NEBULA_HBM_PAGES_PER_GROUP - 1)
                                 / NEBULA_HBM_PAGES_PER_GROUP);

    h->l0_bits = aligned_alloc(NEBULA_BLOCK_SIZE,
                               h->num_pages * NEBULA_BLOCK_SIZE);
    if (!h->l0_bits) { free(h); return -ENOMEM; }

    h->l1_free_counts = calloc(h->num_groups, sizeof(uint64_t));
    if (!h->l1_free_counts) { free(h->l0_bits); free(h); return -ENOMEM; }

    /* Read full bitmap region in one shot. */
    int rc = nebula_io_read(io, bitmap_lba,
                            (uint32_t)h->num_pages, h->l0_bits);
    if (rc != NEBULA_OK) {
        free(h->l1_free_counts); free(h->l0_bits); free(h);
        return rc;
    }

    /* Build L1 summaries: for each page, count allocated bits,
     * and free = NEBULA_BITMAP_BITS_PER_PAGE - allocated. */
    uint64_t total_free = 0;
    for (uint64_t page = 0; page < h->num_pages; page++) {
        const uint8_t *pg = h->l0_bits + page * NEBULA_BLOCK_SIZE;
        uint32_t alloc_bits = 0;
        for (uint32_t i = 0; i < NEBULA_BLOCK_SIZE; i++) {
            alloc_bits += popcount8(pg[i]);
        }
        /* Cap free to remaining blocks in last page. */
        uint64_t page_start_block = page * NEBULA_BITMAP_BITS_PER_PAGE;
        uint64_t page_blocks = NEBULA_BITMAP_BITS_PER_PAGE;
        if (page_start_block + page_blocks > total_blocks) {
            if (page_start_block < total_blocks)
                page_blocks = total_blocks - page_start_block;
            else
                page_blocks = 0;
        }
        uint64_t free_in_page = (alloc_bits >= page_blocks)
                                ? 0
                                : (page_blocks - alloc_bits);

        uint32_t group = (uint32_t)(page / NEBULA_HBM_PAGES_PER_GROUP);
        h->l1_free_counts[group] += free_in_page;
        total_free               += free_in_page;
    }
    h->l2_total_free = total_free;

    *out = h;
    return NEBULA_OK;
}

void nebula_hbm_free(struct nebula_hier_bitmap *h)
{
    if (!h) return;
    free(h->l1_free_counts);
    free(h->l0_bits);
    free(h);
}

uint64_t nebula_hbm_total_free(const struct nebula_hier_bitmap *h)
{
    return h ? h->l2_total_free : 0;
}

void nebula_hbm_print_summary(const struct nebula_hier_bitmap *h)
{
    if (!h) return;
    printf("Hierarchical bitmap:\n");
    printf("  total_blocks:   %lu\n", (unsigned long)h->total_blocks);
    printf("  l0 pages:       %lu\n", (unsigned long)h->num_pages);
    printf("  l1 groups:      %u\n",  h->num_groups);
    printf("  l2 total_free:  %lu blocks (%.2f MiB)\n",
           (unsigned long)h->l2_total_free,
           (double)h->l2_total_free * NEBULA_BLOCK_SIZE / (1024.0*1024.0));
    uint32_t max_show = h->num_groups < 4 ? h->num_groups : 4;
    for (uint32_t g = 0; g < max_show; g++) {
        printf("  group[%u].free: %lu\n", g, (unsigned long)h->l1_free_counts[g]);
    }
}
