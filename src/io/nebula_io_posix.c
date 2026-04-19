/*
 * nebula_io_posix.c - File-backed block I/O via pread/pwrite.
 * Treats any regular file as a block device using 4 KB logical block numbers.
 */
#include "nebula/nebula_io.h"
#include "nebula/nebula_types.h"
#include "../util/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct nebula_io {
    int      fd;
    uint64_t capacity_blocks;
    char    *path;
};

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
            int e = errno;
            close(fd);
            return -e;
        }
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        int e = errno;
        close(fd);
        return -e;
    }

    uint64_t size = (uint64_t)st.st_size;
    if (size < NEBULA_BLOCK_SIZE) {
        close(fd);
        return -EINVAL;
    }

    struct nebula_io *io = calloc(1, sizeof(*io));
    if (!io) {
        close(fd);
        return -ENOMEM;
    }
    io->fd = fd;
    io->capacity_blocks = size / NEBULA_BLOCK_SIZE;
    io->path = strdup(path);

    *out = io;
    return NEBULA_OK;
}

void nebula_io_close(struct nebula_io *io)
{
    if (!io) return;
    if (io->fd >= 0) close(io->fd);
    free(io->path);
    free(io);
}

uint64_t nebula_io_capacity_blocks(const struct nebula_io *io)
{
    return io ? io->capacity_blocks : 0;
}

int nebula_io_read(struct nebula_io *io, nebula_lba_t lba,
                   uint32_t n_blocks, void *buf)
{
    if (!io || !buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    off_t off = (off_t)lba << NEBULA_BLOCK_SHIFT;
    size_t total = (size_t)n_blocks * NEBULA_BLOCK_SIZE;
    size_t done = 0;
    uint8_t *p = (uint8_t *)buf;

    while (done < total) {
        ssize_t r = pread(io->fd, p + done, total - done, off + (off_t)done);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (r == 0) return -EIO;
        done += (size_t)r;
    }
    return NEBULA_OK;
}

int nebula_io_write(struct nebula_io *io, nebula_lba_t lba,
                    uint32_t n_blocks, const void *buf)
{
    if (!io || !buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    off_t off = (off_t)lba << NEBULA_BLOCK_SHIFT;
    size_t total = (size_t)n_blocks * NEBULA_BLOCK_SIZE;
    size_t done = 0;
    const uint8_t *p = (const uint8_t *)buf;

    while (done < total) {
        ssize_t w = pwrite(io->fd, p + done, total - done, off + (off_t)done);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (w == 0) return -EIO;
        done += (size_t)w;
    }
    return NEBULA_OK;
}

int nebula_io_flush(struct nebula_io *io)
{
    if (!io) return -EINVAL;
    if (fsync(io->fd) < 0) return -errno;
    return NEBULA_OK;
}
