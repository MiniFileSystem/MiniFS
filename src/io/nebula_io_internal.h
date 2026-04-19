/*
 * nebula_io_internal.h - Internal vtable definition for I/O backends.
 *
 * Every backend (POSIX, SPDK, ...) defines a static nebula_io_ops and
 * allocates a nebula_io whose first field is a pointer to those ops.
 *
 * Public callers only see the opaque `struct nebula_io *`; they call
 * the thin dispatchers in nebula_io_dispatch.c.
 *
 * Adding a new backend:
 *   1. Allocate  struct nebula_io_impl  (or embed nebula_io as first field).
 *   2. Set       io->ops = &my_backend_ops.
 *   3. Set       io->capacity_blocks.
 *   4. Implement all five ops.
 */
#ifndef NEBULA_IO_INTERNAL_H
#define NEBULA_IO_INTERNAL_H

#include "nebula/nebula_types.h"

struct nebula_io;

/* --- vtable ----------------------------------------------------------- */
struct nebula_io_ops {
    /* Release all resources; impl must free the nebula_io itself. */
    void     (*close)(struct nebula_io *io);

    /* Synchronous 4 KiB-block read.  Returns NEBULA_OK or -errno. */
    int      (*read)(struct nebula_io *io, nebula_lba_t lba,
                     uint32_t n_blocks, void *buf);

    /* Synchronous 4 KiB-block write.  Returns NEBULA_OK or -errno. */
    int      (*write)(struct nebula_io *io, nebula_lba_t lba,
                      uint32_t n_blocks, const void *buf);

    /* Flush / persist writes.  Returns NEBULA_OK or -errno. */
    int      (*flush)(struct nebula_io *io);
};

/* --- base struct every backend embeds --------------------------------- */
struct nebula_io {
    const struct nebula_io_ops *ops;
    uint64_t                    capacity_blocks;
};

#endif /* NEBULA_IO_INTERNAL_H */
