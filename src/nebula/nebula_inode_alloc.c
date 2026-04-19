/*
 * nebula_inode_alloc.c - Inode allocator and read/write helpers.
 *
 * Inode #N lives at LBA  (sb.inode_page_lba + N).
 * Inode #0 is the root inode; nebula_inode_alloc() never returns 0.
 * A free slot has type == NEBULA_INODE_TYPE_FREE (0).
 */
#include "nebula_inode_alloc.h"
#include "nebula_mount.h"
#include "nebula_inode_init.h"   /* nebula_inode_checksum() */
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

static nebula_lba_t inode_lba(const struct nebula_mount *m, uint64_t inode_num)
{
    return (nebula_lba_t)(m->sb.inode_page_lba + inode_num);
}

/* ---------- public API ----------------------------------------------- */

int nebula_inode_read(struct nebula_mount *m, uint64_t inode_num,
                      struct nebula_inode *out)
{
    if (!m || !out) return -EINVAL;
    if (inode_num >= m->sb.inode_page_block_count) {
        NEB_ERR("inode_read: inode %lu out of range (max %lu)",
                (unsigned long)inode_num,
                (unsigned long)m->sb.inode_page_block_count - 1);
        return -EINVAL;
    }

    struct nebula_inode *buf =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    int rc = nebula_io_read(m->io, inode_lba(m, inode_num), 1, buf);
    if (rc != NEBULA_OK) {
        NEB_ERR("inode_read: I/O error reading inode %lu", (unsigned long)inode_num);
        free(buf);
        return rc;
    }

    /* Validate checksum if inode is not free */
    if (buf->type != NEBULA_INODE_TYPE_FREE) {
        uint32_t expected = nebula_inode_checksum(buf);
        if (buf->checksum != expected) {
            NEB_ERR("inode_read: checksum mismatch on inode %lu (got %08x want %08x)",
                    (unsigned long)inode_num, buf->checksum, expected);
            free(buf);
            return -EIO;
        }
    }

    *out = *buf;
    free(buf);
    return NEBULA_OK;
}

int nebula_inode_write(struct nebula_mount *m, struct nebula_inode *ino)
{
    if (!m || !ino) return -EINVAL;
    if (ino->inode_num >= m->sb.inode_page_block_count) {
        NEB_ERR("inode_write: inode %lu out of range",
                (unsigned long)ino->inode_num);
        return -EINVAL;
    }

    struct nebula_inode *buf =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    *buf = *ino;
    buf->checksum = nebula_inode_checksum(buf);
    ino->checksum = buf->checksum;   /* reflect back to caller */

    int rc = nebula_io_write(m->io, inode_lba(m, ino->inode_num), 1, buf);
    if (rc != NEBULA_OK)
        NEB_ERR("inode_write: I/O error writing inode %lu",
                (unsigned long)ino->inode_num);

    free(buf);
    return rc;
}

int nebula_inode_alloc(struct nebula_mount *m, uint32_t type, uint32_t mode,
                       uint64_t *out_inode_num)
{
    if (!m || !out_inode_num) return -EINVAL;
    if (type != NEBULA_INODE_TYPE_FILE && type != NEBULA_INODE_TYPE_DIR)
        return -EINVAL;

    uint64_t total = m->sb.inode_page_block_count;

    struct nebula_inode *buf =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    /* Scan from inode #1; #0 is the root and is never reallocated. */
    for (uint64_t n = 1; n < total; n++) {
        int rc = nebula_io_read(m->io, inode_lba(m, n), 1, buf);
        if (rc != NEBULA_OK) { free(buf); return rc; }

        if (buf->type != NEBULA_INODE_TYPE_FREE)
            continue;

        /* Found a free slot - initialise it. */
        memset(buf, 0, NEBULA_BLOCK_SIZE);
        buf->magic            = (uint32_t)(NEBULA_MAGIC_INODE & 0xFFFFFFFFU);
        buf->version          = 1;
        buf->inode_num        = n;
        buf->type             = type;
        buf->mode             = mode;
        buf->size_bytes       = 0;
        buf->alloc_size_bytes = 0;
        uint64_t t            = now_ns();
        buf->atime_ns         = t;
        buf->mtime_ns         = t;
        buf->ctime_ns         = t;
        buf->nlink            = 1;
        buf->flags            = 0;
        buf->checksum         = nebula_inode_checksum(buf);

        rc = nebula_io_write(m->io, inode_lba(m, n), 1, buf);
        if (rc != NEBULA_OK) {
            NEB_ERR("inode_alloc: I/O error writing inode %lu", (unsigned long)n);
            free(buf);
            return rc;
        }

        NEB_INFO("inode_alloc: allocated inode %lu (type=%u mode=0%o)",
                 (unsigned long)n, type, mode);
        *out_inode_num = n;
        free(buf);
        return NEBULA_OK;
    }

    free(buf);
    NEB_ERR("inode_alloc: no free inode slots (capacity=%lu)", (unsigned long)total);
    return -ENOSPC;
}

int nebula_inode_free(struct nebula_mount *m, uint64_t inode_num)
{
    if (!m) return -EINVAL;
    if (inode_num == NEBULA_ROOT_INODE_NUM) {
        NEB_ERR("inode_free: cannot free root inode");
        return -EINVAL;
    }
    if (inode_num >= m->sb.inode_page_block_count) {
        NEB_ERR("inode_free: inode %lu out of range", (unsigned long)inode_num);
        return -EINVAL;
    }

    struct nebula_inode *buf =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;

    int rc = nebula_io_read(m->io, inode_lba(m, inode_num), 1, buf);
    if (rc != NEBULA_OK) { free(buf); return rc; }

    if (buf->type == NEBULA_INODE_TYPE_FREE) {
        NEB_ERR("inode_free: inode %lu is already free", (unsigned long)inode_num);
        free(buf);
        return -EINVAL;
    }

    /* Zero the block to mark as free */
    memset(buf, 0, NEBULA_BLOCK_SIZE);
    rc = nebula_io_write(m->io, inode_lba(m, inode_num), 1, buf);
    if (rc != NEBULA_OK)
        NEB_ERR("inode_free: I/O error zeroing inode %lu", (unsigned long)inode_num);
    else
        NEB_INFO("inode_free: freed inode %lu", (unsigned long)inode_num);

    free(buf);
    return rc;
}
