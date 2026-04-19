/*
 * nebula_layout.h - Compute device layout (LBA positions of each region).
 */
#ifndef NEBULA_LAYOUT_H
#define NEBULA_LAYOUT_H

#include "nebula_types.h"

/* Valid values for layout.inode_size: 4096 (default) or 8192. */
#define NEBULA_INODE_SIZE_DEFAULT 4096U
#define NEBULA_INODE_SIZE_LARGE   8192U

struct nebula_layout {
    uint64_t     capacity_blocks;
    uint32_t     inode_size;              /* 4096 or 8192 */

    nebula_lba_t mbr_lba;                 /* 0 */
    nebula_lba_t sb_head_lba;             /* 1 */

    nebula_lba_t uberblock_lba;           /* 2 */
    uint64_t     uberblock_count;         /* 128 */

    nebula_lba_t alloc_roots_head_lba;    /* 130 */

    nebula_lba_t bitmap_lba;
    uint64_t     bitmap_block_count;

    nebula_lba_t stream_map_lba;
    uint64_t     stream_map_block_count;

    nebula_lba_t inode_page_lba;
    uint64_t     inode_page_block_count;

    nebula_lba_t dir_page_lba;
    uint64_t     dir_page_block_count;

    nebula_lba_t data_start_lba;
    uint64_t     data_block_count;

    nebula_lba_t alloc_roots_tail_lba;
    nebula_lba_t sb_tail_lba;
};

/* Compute layout from device capacity.
 * Returns NEBULA_OK or -EINVAL if device is too small.
 * Uses the default inode size (4096).
 */
int nebula_layout_compute(uint64_t capacity_blocks, struct nebula_layout *out);

/* Same as nebula_layout_compute but with a configurable inode size.
 * Valid inode_size values: NEBULA_INODE_SIZE_DEFAULT (4096) or
 * NEBULA_INODE_SIZE_LARGE (8192). Any other value returns -EINVAL.
 */
int nebula_layout_compute_ex(uint64_t capacity_blocks, uint32_t inode_size,
                             struct nebula_layout *out);

/* For debug printing */
void nebula_layout_print(const struct nebula_layout *layout);

#endif /* NEBULA_LAYOUT_H */
