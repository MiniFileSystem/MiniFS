/*
 * nebula_root_sub_alloc.h - In-memory helpers for the root inode's
 *                           32 KiB directory sub-allocator.
 *
 * Operates on a 4 KiB nebula_root_chunk_bitmap that lives at the top of
 * each 128 MiB chunk referenced by the root inode's extent map.
 *
 * These functions are pure memory operations -- persisting the bitmap
 * back to disk is the caller's job (will be wired in M3+ with the
 * mkdir / rmdir flows).
 */
#ifndef NEBULA_ROOT_SUB_ALLOC_H
#define NEBULA_ROOT_SUB_ALLOC_H

#include "nebula/nebula_format.h"

/* Initialize a freshly-allocated 128 MiB root chunk bitmap.
 * All 32 KiB slots are marked free, except the first 4 KiB sub-block which
 * is reserved to hold the bitmap page itself.
 */
void nebula_root_chunk_bitmap_init(struct nebula_root_chunk_bitmap *bm);

/* Allocate one free 32 KiB slot.
 * On success returns NEBULA_OK and writes the slot index (0-based within
 * the chunk, 0..NEBULA_ROOT_DIR_SLOTS_PER_CHUNK-1) to *out_slot.
 * Returns -ENOSPC if the chunk is full.
 */
int nebula_root_chunk_alloc_dir(struct nebula_root_chunk_bitmap *bm,
                                uint32_t *out_slot);

/* Free a previously allocated 32 KiB slot. Returns NEBULA_OK or -EINVAL. */
int nebula_root_chunk_free_dir(struct nebula_root_chunk_bitmap *bm,
                               uint32_t slot);

/* Count the number of free 32 KiB slots remaining. */
uint32_t nebula_root_chunk_free_slots(const struct nebula_root_chunk_bitmap *bm);

#endif /* NEBULA_ROOT_SUB_ALLOC_H */
