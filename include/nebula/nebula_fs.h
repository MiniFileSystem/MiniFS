/*
 * nebula_fs.h - Public MiniFS API.
 *
 * Single include for embedding Nebula FS as a library.
 * Callers (e.g. a RocksDB FileSystem adapter or SPDK application) only
 * need to include this header; all internal headers are kept private.
 *
 * Lifecycle:
 *   1. nebula_fs_mount()    - open device, validate metadata, load bitmap.
 *   2. nebula_fs_*()        - file and directory operations.
 *   3. nebula_fs_unmount()  - flush and release resources.
 *
 * Thread-safety: none.  External serialisation required for concurrent access.
 */
#ifndef NEBULA_FS_H
#define NEBULA_FS_H

#include "nebula_types.h"
#include "nebula_io.h"
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- Opaque handles ---- */
typedef struct nebula_mount  nebula_fs_t;      /* mounted device */
typedef struct nebula_file   nebula_fh_t;      /* open file handle */

/* ---- Open flags (OR-able) ---- */
#define NEBULA_FS_O_RDONLY  0x01
#define NEBULA_FS_O_WRONLY  0x02
#define NEBULA_FS_O_RDWR    (NEBULA_FS_O_RDONLY | NEBULA_FS_O_WRONLY)

/* ---- Directory entry info returned by nebula_fs_readdir ---- */
struct nebula_fs_dirent {
    uint64_t inode_num;
    uint16_t flags;          /* NEBULA_DIR_FLAG_* from nebula_format.h */
    char     name[1001];     /* NUL-terminated, max 1000 chars */
};

/* ==================================================================
 * Mount / unmount
 * ================================================================== */

/*
 * Open and mount a Nebula device.
 * path:    filesystem path to device file or block device.
 * out:     receives the mounted handle on success.
 * Returns NEBULA_OK or -errno.
 */
int nebula_fs_mount(const char *path, nebula_fs_t **out);

/*
 * Mount from a pre-created nebula_io handle (e.g. SPDK NVMe backend).
 * The fs takes ownership of io; it will be closed by nebula_fs_unmount.
 * Returns NEBULA_OK or -errno.
 */
int nebula_fs_mount_io(struct nebula_io *io, nebula_fs_t **out);

/*
 * Flush pending state and release all resources.
 */
void nebula_fs_unmount(nebula_fs_t *fs);

/* ==================================================================
 * File operations
 * ================================================================== */

/*
 * Create a new file with the given mode.
 * name:        entry name added to the flat root directory.
 * mode:        Unix permission bits (e.g. 0644).
 * out_fh:      receives an open R/W handle on success.
 * Returns NEBULA_OK, -EEXIST, -ENOSPC, or -EIO.
 */
int nebula_fs_create(nebula_fs_t *fs, const char *name, uint32_t mode,
                     nebula_fh_t **out_fh);

/*
 * Open an existing file by name.
 * flags:  NEBULA_FS_O_RDONLY | NEBULA_FS_O_WRONLY | NEBULA_FS_O_RDWR.
 * Returns NEBULA_OK, -ENOENT, or -EIO.
 */
int nebula_fs_open(nebula_fs_t *fs, const char *name, uint32_t flags,
                   nebula_fh_t **out_fh);

/*
 * Write len bytes from buf at byte offset within the file.
 * Automatically extends the file and allocates blocks as needed.
 * Returns bytes written (>= 0) or -errno.
 */
ssize_t nebula_fs_write(nebula_fh_t *fh, uint64_t offset,
                        const void *buf, size_t len);

/*
 * Read up to len bytes from offset into buf.
 * Unwritten holes are returned as zero bytes.
 * Returns bytes read (0 = EOF) or -errno.
 */
ssize_t nebula_fs_read(nebula_fh_t *fh, uint64_t offset,
                       void *buf, size_t len);

/*
 * Return the current logical size of the file in bytes.
 */
uint64_t nebula_fs_file_size(nebula_fh_t *fh);

/*
 * Flush and close the file handle.  Always safe to call.
 * Returns NEBULA_OK or -EIO if writeback failed.
 */
int nebula_fs_close(nebula_fh_t *fh);

/*
 * Delete a file: remove its directory entry, free all extents, free inode.
 * The handle is invalidated; do not use after this call.
 * Returns NEBULA_OK, -ENOENT, or -EIO.
 */
int nebula_fs_delete(nebula_fs_t *fs, const char *name);

/* ==================================================================
 * Directory operations (flat root namespace)
 * ================================================================== */

/*
 * Look up a name in the root directory.
 * On success, writes inode number to *out_inode_num.
 * Returns NEBULA_OK or -ENOENT.
 */
int nebula_fs_lookup(nebula_fs_t *fs, const char *name,
                     uint64_t *out_inode_num);

/*
 * Iterate over all directory entries.
 * cb is called for each entry; return non-zero from cb to stop early.
 * Returns NEBULA_OK or -EIO.
 */
int nebula_fs_readdir(nebula_fs_t *fs,
                      int (*cb)(const struct nebula_fs_dirent *de, void *ud),
                      void *ud);

/* ==================================================================
 * Utility
 * ================================================================== */

/*
 * Return total and free block counts (4 KiB units).
 */
void nebula_fs_statvfs(nebula_fs_t *fs,
                       uint64_t *out_total_blocks,
                       uint64_t *out_free_blocks);

#endif /* NEBULA_FS_H */
