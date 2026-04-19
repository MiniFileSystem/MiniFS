/*
 * nebula_file.h - File create/open/read/write/delete for Nebula FS.
 *
 * Extent layout (inline, stored in the inode):
 *   Each nebula_extent_map_entry covers a contiguous run of 4 KiB blocks.
 *   em_offset        = logical start of the extent in 4 KiB units.
 *   em_size_and_flags low 24 bits = size in 4 KiB units.
 *   em_lba           = physical start LBA on device.
 *
 * This layer does NOT manage directory entries.  Callers must pair
 * nebula_file_create() with nebula_dir_add() (A8) themselves.
 */
#ifndef NEBULA_FILE_H
#define NEBULA_FILE_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_types.h"
#include <sys/types.h>

struct nebula_mount;

/* Open flags passed to nebula_file_open() / nebula_file_create(). */
#define NEBULA_O_RDONLY  0x01
#define NEBULA_O_WRONLY  0x02
#define NEBULA_O_RDWR    (NEBULA_O_RDONLY | NEBULA_O_WRONLY)

/*
 * In-memory file handle.  Opaque to callers; fields are internal.
 */
struct nebula_file {
    struct nebula_mount *mount;
    struct nebula_inode  inode;   /* cached copy; written back on close/write */
    uint64_t             pos;     /* current byte offset for sequential I/O */
    uint32_t             flags;   /* NEBULA_O_* */
    bool                 dirty;   /* inode needs writeback */
};

/*
 * Create a new regular file inode.
 * Returns NEBULA_OK and writes handle to *out, or -errno.
 * The returned handle is writable.  Caller must call nebula_file_close().
 */
int nebula_file_create(struct nebula_mount *m, uint32_t mode,
                       struct nebula_file **out);

/*
 * Open an existing inode by number.
 * flags must be one of NEBULA_O_RDONLY, NEBULA_O_WRONLY, NEBULA_O_RDWR.
 * Returns NEBULA_OK and writes handle to *out, or -errno.
 */
int nebula_file_open(struct nebula_mount *m, uint64_t inode_num,
                     uint32_t flags, struct nebula_file **out);

/*
 * Flush dirty inode state and free the handle.
 * Always safe to call even if prior I/O failed.
 */
int nebula_file_close(struct nebula_file *f);

/*
 * Write `len` bytes from `buf` at `offset` (bytes) into the file.
 * Allocates new extents as needed.  Updates inode size and mtime.
 * Returns number of bytes written, or -errno.
 */
ssize_t nebula_file_write(struct nebula_file *f, uint64_t offset,
                           const void *buf, size_t len);

/*
 * Read up to `len` bytes from `offset` into `buf`.
 * Reads from allocated extents only; unwritten holes read as zero.
 * Returns number of bytes read (0 = EOF), or -errno.
 */
ssize_t nebula_file_read(struct nebula_file *f, uint64_t offset,
                          void *buf, size_t len);

/*
 * Delete a file: free all extents, then free the inode.
 * The handle is invalidated; do not use it after this call.
 * Returns NEBULA_OK or -errno.
 */
int nebula_file_delete(struct nebula_file *f);

#endif /* NEBULA_FILE_H */
