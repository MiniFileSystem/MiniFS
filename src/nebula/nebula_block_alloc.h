/*
 * nebula_block_alloc.h - Contiguous block allocator backed by the
 * hierarchical bitmap of a mounted device.
 *
 * Current strategy: first-fit linear scan over the L0 bitmap.
 * Every call updates:
 *   - the in-memory L0 bits and L1/L2 summaries on the mount
 *   - the on-disk bitmap page(s) that changed
 *
 * No stream log and no transactions yet - a crash between the write
 * here and the next sync may lose the change.  Crash-safety arrives
 * with the stream log (M3 in the original roadmap).
 */
#ifndef NEBULA_BLOCK_ALLOC_H
#define NEBULA_BLOCK_ALLOC_H

#include "nebula/nebula_types.h"

struct nebula_mount;

/* Allocate `n` contiguous blocks.  On success writes the starting LBA to
 * *out_lba and returns NEBULA_OK.  Returns -ENOSPC if no suitable run
 * exists, -EINVAL for bad args.
 */
int nebula_block_alloc(struct nebula_mount *m, uint32_t n, nebula_lba_t *out_lba);

/* Free `n` contiguous blocks starting at `lba`.  Returns -EINVAL for
 * out-of-range or if the range is already free.
 */
int nebula_block_free(struct nebula_mount *m, nebula_lba_t lba, uint32_t n);

#endif
