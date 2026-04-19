/*
 * nebula_fs.c - Public MiniFS API implementation.
 *
 * Thin shim that wires the public nebula_fs_* interface to the internal
 * mount / file / dir / block-alloc layers.
 */
#include "nebula/nebula_fs.h"
#include "nebula/nebula_format.h"
#include "nebula_mount.h"
#include "nebula_file.h"
#include "nebula_dir.h"
#include "nebula_hier_bitmap.h"
#include "../util/log.h"

#include <errno.h>
#include <string.h>

/* ---- Mount / unmount ---- */

int nebula_fs_mount(const char *path, nebula_fs_t **out)
{
    return nebula_mount_open(path, out);
}

void nebula_fs_unmount(nebula_fs_t *fs)
{
    nebula_mount_unmount(fs);
}

/* ---- File operations ---- */

int nebula_fs_create(nebula_fs_t *fs, const char *name, uint32_t mode,
                     nebula_fh_t **out_fh)
{
    if (!fs || !name || !out_fh) return -EINVAL;

    struct nebula_file *f = NULL;
    int rc = nebula_file_create(fs, mode, &f);
    if (rc != NEBULA_OK) return rc;

    rc = nebula_dir_add(fs, name, f->inode.inode_num, NEBULA_DIR_FLAG_FILE);
    if (rc != NEBULA_OK) {
        nebula_file_delete(f);   /* roll back inode allocation */
        return rc;
    }

    *out_fh = f;
    return NEBULA_OK;
}

int nebula_fs_open(nebula_fs_t *fs, const char *name, uint32_t flags,
                   nebula_fh_t **out_fh)
{
    if (!fs || !name || !out_fh) return -EINVAL;

    uint64_t inum = 0;
    int rc = nebula_dir_lookup(fs, name, &inum);
    if (rc != NEBULA_OK) return rc;

    /* Map public flags to internal flags (same bit values) */
    return nebula_file_open(fs, inum, flags, out_fh);
}

ssize_t nebula_fs_write(nebula_fh_t *fh, uint64_t offset,
                        const void *buf, size_t len)
{
    return nebula_file_write(fh, offset, buf, len);
}

ssize_t nebula_fs_read(nebula_fh_t *fh, uint64_t offset,
                       void *buf, size_t len)
{
    return nebula_file_read(fh, offset, buf, len);
}

uint64_t nebula_fs_file_size(nebula_fh_t *fh)
{
    if (!fh) return 0;
    return fh->inode.size_bytes;
}

int nebula_fs_close(nebula_fh_t *fh)
{
    return nebula_file_close(fh);
}

int nebula_fs_delete(nebula_fs_t *fs, const char *name)
{
    if (!fs || !name) return -EINVAL;

    uint64_t inum = 0;
    int rc = nebula_dir_lookup(fs, name, &inum);
    if (rc != NEBULA_OK) return rc;

    /* Remove directory entry first */
    rc = nebula_dir_remove(fs, name);
    if (rc != NEBULA_OK) return rc;

    /* Open and delete the inode + extents */
    struct nebula_file *f = NULL;
    rc = nebula_file_open(fs, inum, NEBULA_O_RDWR, &f);
    if (rc != NEBULA_OK) return rc;

    return nebula_file_delete(f);   /* frees f */
}

/* ---- Directory operations ---- */

int nebula_fs_lookup(nebula_fs_t *fs, const char *name,
                     uint64_t *out_inode_num)
{
    return nebula_dir_lookup(fs, name, out_inode_num);
}

/* Adapter: bridge nebula_dir_list callback to nebula_fs_readdir callback */
struct readdir_ctx {
    int (*cb)(const struct nebula_fs_dirent *de, void *ud);
    void *ud;
};

static int readdir_bridge(const char *name, uint64_t inode_num,
                          uint16_t flags, void *ud)
{
    struct readdir_ctx *ctx = ud;
    struct nebula_fs_dirent de;
    de.inode_num = inode_num;
    de.flags     = flags;
    size_t nlen  = strlen(name);
    if (nlen >= sizeof(de.name)) nlen = sizeof(de.name) - 1;
    memcpy(de.name, name, nlen);
    de.name[nlen] = '\0';
    return ctx->cb(&de, ctx->ud);
}

int nebula_fs_readdir(nebula_fs_t *fs,
                      int (*cb)(const struct nebula_fs_dirent *de, void *ud),
                      void *ud)
{
    if (!fs || !cb) return -EINVAL;
    struct readdir_ctx ctx = { cb, ud };
    return nebula_dir_list(fs, readdir_bridge, &ctx);
}

/* ---- Utility ---- */

void nebula_fs_statvfs(nebula_fs_t *fs,
                       uint64_t *out_total_blocks,
                       uint64_t *out_free_blocks)
{
    if (!fs) {
        if (out_total_blocks) *out_total_blocks = 0;
        if (out_free_blocks)  *out_free_blocks  = 0;
        return;
    }
    if (out_total_blocks)
        *out_total_blocks = fs->sb.device_capacity_blocks;
    if (out_free_blocks)
        *out_free_blocks  = fs->bitmap ? nebula_hbm_total_free(fs->bitmap) : 0;
}
