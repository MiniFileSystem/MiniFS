/*
 * nebula_inode_alloc.h - Inode allocator and read/write helpers.
 *
 * Inodes are stored one per 4 KiB block in the inode region.
 * Inode #0 is the root directory inode written by mkfs; it is never
 * returned by nebula_inode_alloc().
 *
 * Layout (from superblock):
 *   inode_page_lba + N  =>  inode #N  (0-based)
 *   total inodes        =>  sb.inode_page_block_count
 */
#ifndef NEBULA_INODE_ALLOC_H
#define NEBULA_INODE_ALLOC_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_types.h"

struct nebula_mount;

/*
 * Read inode inode_num from disk into *out.
 * Returns NEBULA_OK, -EINVAL (bad args / out-of-range), or -EIO.
 */
int nebula_inode_read(struct nebula_mount *m, uint64_t inode_num,
                      struct nebula_inode *out);

/*
 * Write inode to disk.  Recomputes checksum before writing.
 * inode->inode_num determines the on-disk position.
 * Returns NEBULA_OK, -EINVAL, or -EIO.
 */
int nebula_inode_write(struct nebula_mount *m, struct nebula_inode *ino);

/*
 * Allocate a new inode of given type (NEBULA_INODE_TYPE_FILE or _DIR)
 * and mode.  Scans the inode region for the first free slot (type==FREE).
 * Initialises all fields and writes to disk.
 * On success, *out_inode_num receives the allocated inode number.
 * Returns NEBULA_OK, -ENOSPC, or -EIO.
 */
int nebula_inode_alloc(struct nebula_mount *m, uint32_t type, uint32_t mode,
                       uint64_t *out_inode_num);

/*
 * Free inode inode_num: zero the inode block on disk and mark type FREE.
 * Returns NEBULA_OK, -EINVAL (bad args / already free / root), or -EIO.
 */
int nebula_inode_free(struct nebula_mount *m, uint64_t inode_num);

#endif /* NEBULA_INODE_ALLOC_H */
