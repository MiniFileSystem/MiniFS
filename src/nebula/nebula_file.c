/*
 * nebula_file.c - File create/open/read/write/delete for Nebula FS.
 *
 * Extent strategy (simple, no hole punching):
 *   - Extents are stored inline in the inode (up to NEBULA_EXTENTS_PER_INODE).
 *   - Each write that falls beyond the current file size appends a new extent.
 *   - Writes within existing extents overwrite in place (no copy-on-write yet).
 *   - Reads walk the sorted extent list and fill holes with zeros.
 *
 * All sizes/offsets are in bytes at the API boundary; internally converted
 * to 4 KiB block units when talking to the block allocator or I/O layer.
 */
#include "nebula_file.h"
#include "nebula_mount.h"
#include "nebula_inode_alloc.h"
#include "nebula_block_alloc.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- helpers -------------------------------------------------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint32_t em_size_blocks(const struct nebula_extent_map_entry *e)
{
    return e->em_size_and_flags & 0x00FFFFFFU;
}

/*
 * Find the extent that covers logical block `blk` (4 KiB unit).
 * Returns index into inode->extents[], or -1 if not found.
 */
static int extent_find(const struct nebula_inode *ino, uint64_t blk)
{
    for (int i = 0; i < (int)NEBULA_EXTENTS_PER_INODE; i++) {
        const struct nebula_extent_map_entry *e = &ino->extents[i];
        if (em_size_blocks(e) == 0) continue;
        uint64_t end = e->em_offset + em_size_blocks(e);
        if (blk >= e->em_offset && blk < end)
            return i;
    }
    return -1;
}

/*
 * Append a new extent to the inode covering `n_blocks` starting at `lba`.
 * `log_blk` is the logical 4 KiB block offset within the file.
 * Returns NEBULA_OK or -ENOSPC (no free extent slots).
 */
static int extent_append(struct nebula_inode *ino,
                         uint64_t log_blk, uint32_t n_blocks, nebula_lba_t lba)
{
    for (int i = 0; i < (int)NEBULA_EXTENTS_PER_INODE; i++) {
        struct nebula_extent_map_entry *e = &ino->extents[i];
        if (em_size_blocks(e) != 0) continue;
        e->em_offset         = log_blk;
        e->em_size_and_flags = n_blocks & 0x00FFFFFFU;
        e->em_lba            = (uint32_t)lba;
        return NEBULA_OK;
    }
    return -ENOSPC;  /* inline extent map full */
}

/* ---------- public API ----------------------------------------------- */

int nebula_file_create(struct nebula_mount *m, uint32_t mode,
                       struct nebula_file **out)
{
    if (!m || !out) return -EINVAL;

    struct nebula_file *f = calloc(1, sizeof(*f));
    if (!f) return -ENOMEM;

    uint64_t inum = 0;
    int rc = nebula_inode_alloc(m, NEBULA_INODE_TYPE_FILE, mode, &inum);
    if (rc != NEBULA_OK) { free(f); return rc; }

    rc = nebula_inode_read(m, inum, &f->inode);
    if (rc != NEBULA_OK) { free(f); return rc; }

    f->mount = m;
    f->flags = NEBULA_O_RDWR;
    f->pos   = 0;
    f->dirty = false;

    *out = f;
    return NEBULA_OK;
}

int nebula_file_open(struct nebula_mount *m, uint64_t inode_num,
                     uint32_t flags, struct nebula_file **out)
{
    if (!m || !out) return -EINVAL;
    if (!(flags & (NEBULA_O_RDONLY | NEBULA_O_WRONLY))) return -EINVAL;

    struct nebula_file *f = calloc(1, sizeof(*f));
    if (!f) return -ENOMEM;

    int rc = nebula_inode_read(m, inode_num, &f->inode);
    if (rc != NEBULA_OK) { free(f); return rc; }

    if (f->inode.type == NEBULA_INODE_TYPE_FREE) {
        NEB_ERR("file_open: inode %lu is free", (unsigned long)inode_num);
        free(f);
        return -ENOENT;
    }

    f->mount = m;
    f->flags = flags;
    f->pos   = 0;
    f->dirty = false;

    *out = f;
    return NEBULA_OK;
}

int nebula_file_close(struct nebula_file *f)
{
    if (!f) return NEBULA_OK;
    int rc = NEBULA_OK;
    if (f->dirty) {
        f->inode.mtime_ns = now_ns();
        rc = nebula_inode_write(f->mount, &f->inode);
        if (rc != NEBULA_OK)
            NEB_ERR("file_close: inode writeback failed for inode %lu",
                    (unsigned long)f->inode.inode_num);
    }
    free(f);
    return rc;
}

ssize_t nebula_file_write(struct nebula_file *f, uint64_t offset,
                           const void *buf, size_t len)
{
    if (!f || !buf) return -EINVAL;
    if (!(f->flags & NEBULA_O_WRONLY)) return -EBADF;
    if (len == 0) return 0;

    struct nebula_mount *m = f->mount;
    const uint8_t *src = buf;
    size_t written = 0;

    while (written < len) {
        uint64_t cur_off  = offset + written;
        uint64_t blk      = cur_off / NEBULA_BLOCK_SIZE;   /* logical block */
        uint32_t blk_off  = (uint32_t)(cur_off % NEBULA_BLOCK_SIZE);
        size_t   to_write = NEBULA_BLOCK_SIZE - blk_off;
        if (to_write > len - written) to_write = len - written;

        int idx = extent_find(&f->inode, blk);
        nebula_lba_t phys_lba;

        if (idx < 0) {
            /* Need to allocate a new extent.  Round up remaining bytes to
             * whole blocks and allocate them all in one shot, capped at 256
             * blocks (1 MiB) to avoid huge single allocations. */
            uint64_t bytes_left = len - written;
            uint32_t need_blks  = (uint32_t)((bytes_left + blk_off +
                                               NEBULA_BLOCK_SIZE - 1) /
                                              NEBULA_BLOCK_SIZE);
            if (need_blks > 256) need_blks = 256;
            if (need_blks == 0)  need_blks = 1;

            int rc = nebula_block_alloc(m, need_blks, &phys_lba);
            if (rc != NEBULA_OK) {
                NEB_ERR("file_write: block alloc failed: %s", strerror(-rc));
                return written > 0 ? (ssize_t)written : rc;
            }

            rc = extent_append(&f->inode, blk, need_blks, phys_lba);
            if (rc != NEBULA_OK) {
                nebula_block_free(m, phys_lba, need_blks);
                NEB_ERR("file_write: extent map full");
                return written > 0 ? (ssize_t)written : rc;
            }
            f->dirty = true;
        } else {
            const struct nebula_extent_map_entry *e = &f->inode.extents[idx];
            phys_lba = (nebula_lba_t)(e->em_lba + (blk - e->em_offset));
        }

        /* Read-modify-write if writing a partial block */
        void *block_buf = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
        if (!block_buf) return written > 0 ? (ssize_t)written : -ENOMEM;

        if (blk_off != 0 || to_write < NEBULA_BLOCK_SIZE) {
            int rc = nebula_io_read(m->io, phys_lba, 1, block_buf);
            if (rc != NEBULA_OK) {
                free(block_buf);
                return written > 0 ? (ssize_t)written : rc;
            }
        } else {
            memset(block_buf, 0, NEBULA_BLOCK_SIZE);
        }

        memcpy((uint8_t *)block_buf + blk_off, src + written, to_write);

        int rc = nebula_io_write(m->io, phys_lba, 1, block_buf);
        free(block_buf);
        if (rc != NEBULA_OK) {
            NEB_ERR("file_write: I/O error at LBA %lu", (unsigned long)phys_lba);
            return written > 0 ? (ssize_t)written : rc;
        }

        written += to_write;
        f->dirty = true;
    }

    /* Update logical size */
    if (offset + written > f->inode.size_bytes) {
        f->inode.size_bytes = offset + written;
        /* alloc_size_bytes tracks blocks actually handed out */
        f->inode.alloc_size_bytes = 0;
        for (int i = 0; i < (int)NEBULA_EXTENTS_PER_INODE; i++) {
            uint32_t sz = em_size_blocks(&f->inode.extents[i]);
            f->inode.alloc_size_bytes += (uint64_t)sz * NEBULA_BLOCK_SIZE;
        }
    }

    /* Flush inode on every write so the size is durable */
    f->inode.mtime_ns = now_ns();
    int rc = nebula_inode_write(m, &f->inode);
    if (rc != NEBULA_OK) {
        NEB_ERR("file_write: inode flush failed");
        return written > 0 ? (ssize_t)written : rc;
    }
    f->dirty = false;

    return (ssize_t)written;
}

ssize_t nebula_file_read(struct nebula_file *f, uint64_t offset,
                          void *buf, size_t len)
{
    if (!f || !buf) return -EINVAL;
    if (!(f->flags & NEBULA_O_RDONLY)) return -EBADF;
    if (len == 0) return 0;

    /* Clamp to file size */
    if (offset >= f->inode.size_bytes) return 0;
    if (offset + len > f->inode.size_bytes)
        len = (size_t)(f->inode.size_bytes - offset);

    struct nebula_mount *m = f->mount;
    uint8_t *dst   = buf;
    size_t  done   = 0;

    while (done < len) {
        uint64_t cur_off = offset + done;
        uint64_t blk     = cur_off / NEBULA_BLOCK_SIZE;
        uint32_t blk_off = (uint32_t)(cur_off % NEBULA_BLOCK_SIZE);
        size_t   to_read = NEBULA_BLOCK_SIZE - blk_off;
        if (to_read > len - done) to_read = len - done;

        int idx = extent_find(&f->inode, blk);
        if (idx < 0) {
            /* Sparse / unwritten block — return zeros */
            memset(dst + done, 0, to_read);
        } else {
            const struct nebula_extent_map_entry *e = &f->inode.extents[idx];
            nebula_lba_t phys_lba =
                (nebula_lba_t)(e->em_lba + (blk - e->em_offset));

            void *block_buf =
                aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
            if (!block_buf) return done > 0 ? (ssize_t)done : -ENOMEM;

            int rc = nebula_io_read(m->io, phys_lba, 1, block_buf);
            if (rc != NEBULA_OK) {
                free(block_buf);
                return done > 0 ? (ssize_t)done : rc;
            }

            memcpy(dst + done, (uint8_t *)block_buf + blk_off, to_read);
            free(block_buf);
        }

        done += to_read;
    }

    f->inode.atime_ns = now_ns();
    /* atime update is best-effort; don't write inode back on every read */

    return (ssize_t)done;
}

int nebula_file_delete(struct nebula_file *f)
{
    if (!f) return -EINVAL;

    struct nebula_mount *m = f->mount;
    int rc = NEBULA_OK;

    /* Free all extents */
    for (int i = 0; i < (int)NEBULA_EXTENTS_PER_INODE; i++) {
        struct nebula_extent_map_entry *e = &f->inode.extents[i];
        uint32_t sz = em_size_blocks(e);
        if (sz == 0) continue;

        int frc = nebula_block_free(m, (nebula_lba_t)e->em_lba, sz);
        if (frc != NEBULA_OK) {
            NEB_ERR("file_delete: block_free LBA %u n=%u: %s",
                    e->em_lba, sz, strerror(-frc));
            rc = frc;  /* keep going; free as much as possible */
        }
        memset(e, 0, sizeof(*e));
    }

    /* Free the inode */
    int frc = nebula_inode_free(m, f->inode.inode_num);
    if (frc != NEBULA_OK && rc == NEBULA_OK) rc = frc;

    free(f);
    return rc;
}
