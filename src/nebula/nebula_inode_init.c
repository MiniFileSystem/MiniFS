/*
 * nebula_inode_init.c - Initialize inode and directory pages.
 *
 * Inode page: zero-fill whole region, then write root inode (#0) at LBA
 * inode_page_lba. Root inode is type=DIR.
 *
 * Directory page: zero-fill, write header with "/" -> 0 entry in first
 * content block.
 */
#include "nebula_inode_init.h"
#include "../util/crc32c.h"
#include "../util/log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------- Inode ---------- */

uint32_t nebula_inode_checksum(const struct nebula_inode *ino)
{
    struct nebula_inode tmp = *ino;
    tmp.checksum = 0;
    return crc32c(&tmp, sizeof(tmp));
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int zero_region(struct nebula_io *io, nebula_lba_t lba, uint64_t n_blocks)
{
    enum { BATCH = 64 };
    void *buf = aligned_alloc(NEBULA_BLOCK_SIZE, BATCH * NEBULA_BLOCK_SIZE);
    if (!buf) return -ENOMEM;
    memset(buf, 0, BATCH * NEBULA_BLOCK_SIZE);

    uint64_t remaining = n_blocks;
    nebula_lba_t cur = lba;
    while (remaining > 0) {
        uint32_t n = remaining > BATCH ? (uint32_t)BATCH : (uint32_t)remaining;
        int rc = nebula_io_write(io, cur, n, buf);
        if (rc != NEBULA_OK) { free(buf); return rc; }
        cur += n; remaining -= n;
    }
    free(buf);
    return NEBULA_OK;
}

int nebula_inode_page_init(struct nebula_io *io, const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    /* Zero entire inode region first */
    int rc = zero_region(io, L->inode_page_lba, L->inode_page_block_count);
    if (rc != NEBULA_OK) return rc;

    /* Write root inode at offset 0 (= inode #0). */
    struct nebula_inode *ino = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!ino) return -ENOMEM;
    memset(ino, 0, NEBULA_BLOCK_SIZE);

    ino->magic            = (uint32_t)(NEBULA_MAGIC_INODE & 0xFFFFFFFF);
    ino->version          = 1;
    ino->inode_num        = NEBULA_ROOT_INODE_NUM;
    ino->type             = NEBULA_INODE_TYPE_DIR;
    ino->mode             = 0755;
    ino->size_bytes       = 0;
    ino->alloc_size_bytes = 0;
    ino->atime_ns = ino->mtime_ns = ino->ctime_ns = now_ns();
    ino->nlink            = 1;
    ino->flags            = 0;
    ino->checksum         = nebula_inode_checksum(ino);

    rc = nebula_io_write(io, L->inode_page_lba, 1, ino);
    free(ino);
    return rc;
}

/* ---------- Directory page ---------- */

static uint64_t fnv1a_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint64_t)(uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

int nebula_dir_page_init(struct nebula_io *io, const struct nebula_layout *L)
{
    if (!io || !L) return -EINVAL;

    /* Zero entire directory region. */
    int rc = zero_region(io, L->dir_page_lba, L->dir_page_block_count);
    if (rc != NEBULA_OK) return rc;

    /* Build header block. */
    struct nebula_dir_page_header *hdr =
        aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!hdr) return -ENOMEM;
    memset(hdr, 0, NEBULA_BLOCK_SIZE);

    uint64_t total_blocks = L->dir_page_block_count;
    uint64_t capacity     = (total_blocks - 1) * NEBULA_DIR_ENTRIES_PER_BLOCK;

    hdr->magic       = NEBULA_MAGIC_DIR_PAGE;
    hdr->version     = 1;
    hdr->entry_size  = NEBULA_DIR_ENTRY_SIZE;
    hdr->capacity    = capacity;
    hdr->num_entries = 1;  /* "/" */
    {
        struct nebula_dir_page_header tmp = *hdr;
        tmp.checksum = 0;
        hdr->checksum = crc32c(&tmp, sizeof(tmp));
    }
    rc = nebula_io_write(io, L->dir_page_lba, 1, hdr);
    free(hdr);
    if (rc != NEBULA_OK) return rc;

    /* Write first content block containing one entry for "/". */
    void *block = aligned_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!block) return -ENOMEM;
    memset(block, 0, NEBULA_BLOCK_SIZE);

    struct nebula_dir_entry *de = block;
    de->hash      = fnv1a_hash("/");
    de->inode_num = NEBULA_ROOT_INODE_NUM;
    de->name_len  = 1;
    de->flags     = NEBULA_DIR_FLAG_USED | NEBULA_DIR_FLAG_DIR;
    de->name[0]   = '/';

    rc = nebula_io_write(io, L->dir_page_lba + 1, 1, block);
    free(block);
    return rc;
}
