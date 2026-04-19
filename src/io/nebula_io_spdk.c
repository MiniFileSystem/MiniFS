/*
 * nebula_io_spdk.c - SPDK NVMe nebula_io backend.
 *
 * Implements the nebula_io_ops vtable using nebula_spdk_nvme_* as the
 * underlying transport.  All I/O buffers passed through this layer MUST
 * be DMA-safe; the backend transparently copies non-DMA buffers through
 * a per-handle bounce buffer (B5: DMA buffer pool) to remain compatible
 * with callers that use stack or heap allocations.
 */
#include "nebula_io_spdk.h"
#include "nebula_io_internal.h"
#include "nebula_spdk_nvme.h"
#include "nebula_spdk_env.h"
#include "nebula/nebula_types.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Maximum single-call transfer: 256 × 4 KiB = 1 MiB */
#define SPDK_BOUNCE_BLOCKS 256U
#define SPDK_BOUNCE_BYTES  (SPDK_BOUNCE_BLOCKS * NEBULA_BLOCK_SIZE)

/* ---------- SPDK-specific extension of the base struct --------------- */

struct nebula_io_spdk {
    struct nebula_io       base;     /* MUST be first */
    struct nebula_spdk_ns *ns;
    void                  *bounce;  /* DMA-safe bounce buffer (SPDK_BOUNCE_BYTES) */
};

/* ---------- vtable implementations ------------------------------------ */

static void spdk_io_close(struct nebula_io *io)
{
    struct nebula_io_spdk *s = (struct nebula_io_spdk *)io;
    if (s->bounce) nebula_spdk_dma_free(s->bounce);
    if (s->ns)     nebula_spdk_nvme_close(s->ns);
    free(s);
}

static int spdk_io_read(struct nebula_io *io, nebula_lba_t lba,
                        uint32_t n_blocks, void *buf)
{
    struct nebula_io_spdk *s = (struct nebula_io_spdk *)io;
    if (!buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    uint32_t remaining = n_blocks;
    uint32_t done      = 0;

    while (remaining > 0) {
        uint32_t batch = remaining < SPDK_BOUNCE_BLOCKS
                         ? remaining : SPDK_BOUNCE_BLOCKS;

        int rc = nebula_spdk_nvme_read(s->ns, lba + done, batch, s->bounce);
        if (rc != 0) return -EIO;

        memcpy((uint8_t *)buf + (size_t)done * NEBULA_BLOCK_SIZE,
               s->bounce, (size_t)batch * NEBULA_BLOCK_SIZE);

        done      += batch;
        remaining -= batch;
    }
    return NEBULA_OK;
}

static int spdk_io_write(struct nebula_io *io, nebula_lba_t lba,
                         uint32_t n_blocks, const void *buf)
{
    struct nebula_io_spdk *s = (struct nebula_io_spdk *)io;
    if (!buf) return -EINVAL;
    if (lba + n_blocks > io->capacity_blocks) return -ERANGE;

    uint32_t remaining = n_blocks;
    uint32_t done      = 0;

    while (remaining > 0) {
        uint32_t batch = remaining < SPDK_BOUNCE_BLOCKS
                         ? remaining : SPDK_BOUNCE_BLOCKS;

        memcpy(s->bounce,
               (const uint8_t *)buf + (size_t)done * NEBULA_BLOCK_SIZE,
               (size_t)batch * NEBULA_BLOCK_SIZE);

        int rc = nebula_spdk_nvme_write(s->ns, lba + done, batch, s->bounce);
        if (rc != 0) return -EIO;

        done      += batch;
        remaining -= batch;
    }
    return NEBULA_OK;
}

static int spdk_io_flush(struct nebula_io *io)
{
    struct nebula_io_spdk *s = (struct nebula_io_spdk *)io;
    return nebula_spdk_nvme_flush(s->ns) == 0 ? NEBULA_OK : -EIO;
}

static const struct nebula_io_ops spdk_ops = {
    .close = spdk_io_close,
    .read  = spdk_io_read,
    .write = spdk_io_write,
    .flush = spdk_io_flush,
};

/* ---------- public factory ------------------------------------------- */

int nebula_io_spdk_open(const char *traddr, struct nebula_io **out)
{
    if (!out) return -EINVAL;

    struct nebula_spdk_ns *ns = NULL;
    if (nebula_spdk_nvme_probe(traddr, &ns) != 0) return -ENODEV;

    /* Allocate DMA bounce buffer */
    void *bounce = nebula_spdk_dma_alloc(SPDK_BOUNCE_BYTES, NEBULA_BLOCK_SIZE);
    if (!bounce) {
        nebula_spdk_nvme_close(ns);
        return -ENOMEM;
    }

    struct nebula_io_spdk *s = calloc(1, sizeof(*s));
    if (!s) {
        nebula_spdk_dma_free(bounce);
        nebula_spdk_nvme_close(ns);
        return -ENOMEM;
    }

    s->base.ops             = &spdk_ops;
    s->base.capacity_blocks = nebula_spdk_nvme_capacity_blocks(ns);
    s->ns                   = ns;
    s->bounce               = bounce;

    *out = &s->base;
    return NEBULA_OK;
}
