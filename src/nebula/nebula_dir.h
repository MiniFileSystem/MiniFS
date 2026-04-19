/*
 * nebula_dir.h - Directory entry add / lookup / remove helpers.
 *
 * The directory region is laid out as:
 *   dir_page_lba + 0    : nebula_dir_page_header (capacity, num_entries, ...)
 *   dir_page_lba + 1..N : content blocks, each holding
 *                         NEBULA_DIR_ENTRIES_PER_BLOCK (4) entries of 1 KiB.
 *
 * Entry slot index S maps to:
 *   LBA   = dir_page_lba + 1 + S / NEBULA_DIR_ENTRIES_PER_BLOCK
 *   Slot  = S % NEBULA_DIR_ENTRIES_PER_BLOCK
 *
 * A used entry has (flags & NEBULA_DIR_FLAG_USED) set.
 * name_len == 0 entries with USED clear are free slots.
 */
#ifndef NEBULA_DIR_H
#define NEBULA_DIR_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_types.h"

struct nebula_mount;

/*
 * Add an entry for `name` -> `inode_num` with type flags (NEBULA_DIR_FLAG_FILE
 * or NEBULA_DIR_FLAG_DIR).
 * Returns NEBULA_OK, -ENOSPC (no free slot), -EEXIST, or -EIO.
 */
int nebula_dir_add(struct nebula_mount *m, const char *name,
                   uint64_t inode_num, uint16_t type_flag);

/*
 * Look up `name` in the directory region.
 * On success, writes the inode number to *out_inode_num and returns NEBULA_OK.
 * Returns -ENOENT if not found.
 */
int nebula_dir_lookup(struct nebula_mount *m, const char *name,
                      uint64_t *out_inode_num);

/*
 * Remove the entry for `name`.
 * Returns NEBULA_OK or -ENOENT.
 */
int nebula_dir_remove(struct nebula_mount *m, const char *name);

/*
 * List all used entries.  Calls cb(name, inode_num, flags, userdata) for
 * each entry; stops and returns early if cb returns non-zero.
 * Returns NEBULA_OK or -EIO.
 */
int nebula_dir_list(struct nebula_mount *m,
                    int (*cb)(const char *name, uint64_t inode_num,
                              uint16_t flags, void *ud),
                    void *ud);

#endif /* NEBULA_DIR_H */
