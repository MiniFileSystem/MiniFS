/*
 * nebula_io_posix.c - File-backed block I/O via pread/pwrite.
 *
 * Implements the nebula_io_ops vtable for a regular file (or block device)
 * opened via the POSIX API.  The public nebula_io_open() entry point here
 * is the default factory used when SPDK is not available.
 */
#include "nebula/nebula_io.h"
#include "nebula/nebula_types.h"
#include "nebula_io_internal.h"
#include "../util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* --- POSIX-specific extension of the base struct ---------------------- */
struct nebula_io_posix {
    struct nebula_io base;   /* MUST be first */
    int              fd;
    char            *path;
};

/* --- vtable implementations ------------------------------------------- */

static void posix_close(struct nebula_io *io)
{
    struct nebula_io_posix *p = (struct nebula_io_posix *)io;
    if (p->fd >= 0) close(p->fd);
    free(p->path);
    free(p);
}

static int posix_read(struct nebula_io *io, nebula_lba_t lba,
                      uint32_t n_blocks, void *buf)
{
    struct nebula_io_posix *p = (struct nebula_io_posix *)io;
    if (!buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    off_t   off   = (off_t)lba << NEBULA_BLOCK_SHIFT;
    size_t  total = (size_t)n_blocks * NEBULA_BLOCK_SIZE;
    size_t  done  = 0;
    uint8_t *dst  = (uint8_t *)buf;

    while (done < total) {
        ssize_t r = pread(p->fd, dst + done, total - done, off + (off_t)done);
        if (r < 0) { if (errno == EINTR) continue; return -errno; }
        if (r == 0) return -EIO;
        done += (size_t)r;
    }
    return NEBULA_OK;
}

static int posix_write(struct nebula_io *io, nebula_lba_t lba,
                       uint32_t n_blocks, const void *buf)
{
    struct nebula_io_posix *p = (struct nebula_io_posix *)io;
    if (!buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    off_t         off   = (off_t)lba << NEBULA_BLOCK_SHIFT;
    size_t        total = (size_t)n_blocks * NEBULA_BLOCK_SIZE;
    size_t        done  = 0;
    const uint8_t *src  = (const uint8_t *)buf;

    while (done < total) {
        ssize_t w = pwrite(p->fd, src + done, total - done, off + (off_t)done);
        if (w < 0) { if (errno == EINTR) continue; return -errno; }
        if (w == 0) return -EIO;
        done += (size_t)w;
    }
    return NEBULA_OK;
}

static int posix_flush(struct nebula_io *io)
{
    struct nebula_io_posix *p = (struct nebula_io_posix *)io;
    if (fsync(p->fd) < 0) return -errno;
    return NEBULA_OK;
}

static const struct nebula_io_ops posix_ops = {
    .close = posix_close,
    .read  = posix_read,
    .write = posix_write,
    .flush = posix_flush,
};

/* --- Public factory ---------------------------------------------------- */

int nebula_io_open(const char *path, bool create, uint64_t size_bytes,
                   struct nebula_io **out)
{
    if (!path || !out) return -EINVAL;

    int flags = O_RDWR;
    if (create) flags |= O_CREAT;

    int fd = open(path, flags, 0644);
    if (fd < 0) return -errno;

    if (create && size_bytes > 0) {
        if (ftruncate(fd, (off_t)size_bytes) < 0) {
            int e = errno; close(fd); return -e;
        }
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int e = errno; close(fd); return -e;
    }

    uint64_t size = (uint64_t)st.st_size;
    if (size < NEBULA_BLOCK_SIZE) { close(fd); return -EINVAL; }

    struct nebula_io_posix *p = calloc(1, sizeof(*p));
    if (!p) { close(fd); return -ENOMEM; }

    p->base.ops            = &posix_ops;
    p->base.capacity_blocks = size / NEBULA_BLOCK_SIZE;
    p->fd                  = fd;
    p->path                = strdup(path);

    *out = &p->base;
    return NEBULA_OK;
}

/* --- Public dispatchers (thin wrappers around vtable) ----------------- */

void nebula_io_close(struct nebula_io *io)
{
    if (io) io->ops->close(io);
}

uint64_t nebula_io_capacity_blocks(const struct nebula_io *io)
{
    return io ? io->capacity_blocks : 0;
}

int nebula_io_read(struct nebula_io *io, nebula_lba_t lba,
                   uint32_t n_blocks, void *buf)
{
    if (!io) return -EINVAL;
    return io->ops->read(io, lba, n_blocks, buf);
}

int nebula_io_write(struct nebula_io *io, nebula_lba_t lba,
                    uint32_t n_blocks, const void *buf)
{
    if (!io) return -EINVAL;
    return io->ops->write(io, lba, n_blocks, buf);
}

int nebula_io_flush(struct nebula_io *io)
{
    if (!io) return -EINVAL;
    return io->ops->flush(io);
}
