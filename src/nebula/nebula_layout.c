/*
 * nebula_layout.c - Compute device region layout.
 *
 * Layout order (LBAs in 4 KB blocks):
 *   0                           MBR
 *   1                           SB head
 *   2 .. 129                    Uberblock region (128 slots)
 *   130                         Alloc roots head (all 64 in one block)
 *   131 .. B-1                  Bitmap pages
 *   B .. S-1                    Stream map region
 *   S .. I-1                    Inode page
 *   I .. D-1                    Directory page
 *   D .. T-3                    Data region
 *   T-2                         Alloc roots tail
 *   T-1                         SB tail
 */
#include "nebula/nebula_layout.h"
#include "nebula/nebula_format.h"
#include "../util/log.h"

#include <stdio.h>
#include <errno.h>

/* Helper: ceiling division */
static inline uint64_t div_ceil(uint64_t a, uint64_t b) { return (a + b - 1) / b; }

static inline uint64_t clamp_u64(uint64_t v, uint64_t lo, uint64_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int nebula_layout_compute_ex(uint64_t capacity_blocks, uint32_t inode_size,
                             struct nebula_layout *out)
{
    if (!out) return -EINVAL;
    if (capacity_blocks < NEBULA_MIN_DEVICE_BYTES / NEBULA_BLOCK_SIZE) {
        return -EINVAL;
    }
    if (inode_size != NEBULA_INODE_SIZE_DEFAULT &&
        inode_size != NEBULA_INODE_SIZE_LARGE) {
        return -EINVAL;
    }

    struct nebula_layout L;
    L.capacity_blocks = capacity_blocks;
    L.inode_size      = inode_size;

    /* Fixed head metadata */
    L.mbr_lba             = NEBULA_LBA_MBR;             /* 0 */
    L.sb_head_lba         = NEBULA_LBA_SB_HEAD;         /* 1 */
    L.uberblock_lba       = NEBULA_LBA_UBERBLOCK;       /* 2 */
    L.uberblock_count     = NEBULA_UBERBLOCK_SLOTS;     /* 128 */
    L.alloc_roots_head_lba = NEBULA_LBA_ALLOC_ROOTS_H;  /* 130 */

    nebula_lba_t cursor = L.alloc_roots_head_lba + 1;   /* 131 */

    /* Bitmap: enough pages to cover all blocks. */
    L.bitmap_lba = cursor;
    L.bitmap_block_count = div_ceil(capacity_blocks, NEBULA_BITMAP_BITS_PER_PAGE);
    cursor += L.bitmap_block_count;

    /* Stream map: 1 MiB = 256 blocks (fixed for milestone 1). */
    L.stream_map_lba = cursor;
    L.stream_map_block_count = 256;
    cursor += L.stream_map_block_count;

    /* Inode page: clamp(capacity/8, 32 MiB, 128 MiB).
     * Design says 128 MiB to 1 GiB; we scale for small test images.
     */
    uint64_t scaled_inode = capacity_blocks / 8;
    L.inode_page_block_count = clamp_u64(
        scaled_inode,
        32ULL * 1024 * 1024 / NEBULA_BLOCK_SIZE,   /* 32 MiB floor */
        128ULL * 1024 * 1024 / NEBULA_BLOCK_SIZE); /* 128 MiB cap */
    L.inode_page_lba = cursor;
    cursor += L.inode_page_block_count;

    /* Directory page: same sizing. */
    L.dir_page_block_count = L.inode_page_block_count;
    L.dir_page_lba = cursor;
    cursor += L.dir_page_block_count;

    /* Reserve last two blocks for tail metadata. */
    if (cursor + 2 > capacity_blocks) {
        return -EINVAL;  /* not enough room for data after metadata */
    }
    L.sb_tail_lba          = capacity_blocks - 1;
    L.alloc_roots_tail_lba = capacity_blocks - 2;

    L.data_start_lba  = cursor;
    L.data_block_count = L.alloc_roots_tail_lba - cursor;

    *out = L;
    return NEBULA_OK;
}

int nebula_layout_compute(uint64_t capacity_blocks, struct nebula_layout *out)
{
    return nebula_layout_compute_ex(capacity_blocks,
                                    NEBULA_INODE_SIZE_DEFAULT, out);
}

void nebula_layout_print(const struct nebula_layout *L)
{
    if (!L) return;
    printf("Layout (capacity=%lu blocks, %.2f MiB, inode_size=%u):\n",
           (unsigned long)L->capacity_blocks,
           (double)L->capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0),
           L->inode_size);
    printf("  %-22s %10lu\n", "MBR lba",                (unsigned long)L->mbr_lba);
    printf("  %-22s %10lu\n", "SB head lba",            (unsigned long)L->sb_head_lba);
    printf("  %-22s %10lu (x%lu slots)\n", "Uberblock lba",
           (unsigned long)L->uberblock_lba, (unsigned long)L->uberblock_count);
    printf("  %-22s %10lu\n", "Alloc roots head lba",   (unsigned long)L->alloc_roots_head_lba);
    printf("  %-22s %10lu (x%lu blocks)\n", "Bitmap lba",
           (unsigned long)L->bitmap_lba, (unsigned long)L->bitmap_block_count);
    printf("  %-22s %10lu (x%lu blocks)\n", "Stream map lba",
           (unsigned long)L->stream_map_lba, (unsigned long)L->stream_map_block_count);
    printf("  %-22s %10lu (x%lu blocks)\n", "Inode page lba",
           (unsigned long)L->inode_page_lba, (unsigned long)L->inode_page_block_count);
    printf("  %-22s %10lu (x%lu blocks)\n", "Dir page lba",
           (unsigned long)L->dir_page_lba, (unsigned long)L->dir_page_block_count);
    printf("  %-22s %10lu (x%lu blocks)\n", "Data start lba",
           (unsigned long)L->data_start_lba, (unsigned long)L->data_block_count);
    printf("  %-22s %10lu\n", "Alloc roots tail lba",   (unsigned long)L->alloc_roots_tail_lba);
    printf("  %-22s %10lu\n", "SB tail lba",            (unsigned long)L->sb_tail_lba);
}
