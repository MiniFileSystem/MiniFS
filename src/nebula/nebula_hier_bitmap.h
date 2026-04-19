/*
 * nebula_hier_bitmap.h - In-memory hierarchical free-space bitmap.
 *
 * Level 0: raw per-block bits (from on-disk bitmap pages).
 * Level 1: free count per group (PAGES_PER_GROUP bitmap pages).
 * Level 2: total free count.
 *
 * Bit value: 0 = free, 1 = allocated.
 */
#ifndef NEBULA_HIER_BITMAP_H
#define NEBULA_HIER_BITMAP_H

#include "nebula/nebula_types.h"

#define NEBULA_HBM_PAGES_PER_GROUP 64U

struct nebula_hier_bitmap {
    uint64_t  total_blocks;
    uint64_t  num_pages;        /* L0: bitmap pages covering total_blocks */
    uint8_t  *l0_bits;          /* num_pages * NEBULA_BLOCK_SIZE */
    uint32_t  num_groups;       /* L1: ceil(num_pages / PAGES_PER_GROUP) */
    uint64_t *l1_free_counts;   /* free blocks per group */
    uint64_t  l2_total_free;    /* sum */
};

/* Allocate and load from device. Walks bitmap pages, populates L0/L1/L2. */
struct nebula_io;
int nebula_hbm_load(struct nebula_io *io,
                    nebula_lba_t bitmap_lba,
                    uint64_t bitmap_block_count,
                    uint64_t total_blocks,
                    struct nebula_hier_bitmap **out);

void nebula_hbm_free(struct nebula_hier_bitmap *hbm);

/* Return total free blocks (L2 summary). */
uint64_t nebula_hbm_total_free(const struct nebula_hier_bitmap *hbm);

/* Debug print: first few non-empty groups. */
void nebula_hbm_print_summary(const struct nebula_hier_bitmap *hbm);

#endif
