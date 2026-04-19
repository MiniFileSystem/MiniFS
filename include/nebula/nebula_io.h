/*
 * nebula_io.h - Backend-agnostic block I/O interface.
 *
 * All offsets are in 4 KB block numbers (nebula_lba_t).
 * All buffers should be 4 KB aligned for best performance.
 */
#ifndef NEBULA_IO_H
#define NEBULA_IO_H

#include "nebula_types.h"

struct nebula_io;

/* Open device-backing storage.
 *   path:   filesystem path (file-backed backend).
 *   create: if true, create file if absent; will be sized if size_bytes > 0.
 *   size_bytes: if create=true, truncate/grow the file to this size.
 * On success returns NEBULA_OK and writes handle to *out.
 * On failure returns -errno.
 */
int nebula_io_open(const char *path, bool create, uint64_t size_bytes,
                   struct nebula_io **out);

void nebula_io_close(struct nebula_io *io);

/* Device capacity in 4 KB blocks. */
uint64_t nebula_io_capacity_blocks(const struct nebula_io *io);

/* Read/write n_blocks of 4 KB at given LBA. Returns NEBULA_OK or -errno. */
int nebula_io_read(struct nebula_io *io, nebula_lba_t lba,
                   uint32_t n_blocks, void *buf);
int nebula_io_write(struct nebula_io *io, nebula_lba_t lba,
                    uint32_t n_blocks, const void *buf);

/* Flush to stable storage. */
int nebula_io_flush(struct nebula_io *io);

#endif /* NEBULA_IO_H */
