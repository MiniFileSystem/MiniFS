/*
 * nebula_spdk_nvme.c - NVMe controller probe, namespace and synchronous I/O.
 *
 * Strategy:
 *   - Use spdk_nvme_probe() to attach to NVMe controllers.
 *   - Select the first namespace that is active and has sector_size <= 4096.
 *   - Create one I/O qpair per nebula_spdk_ns handle.
 *   - Wrap async SPDK completions in a poll loop to present a synchronous API.
 *
 * 4 KiB alignment:
 *   NVMe devices may have 512-byte or 4096-byte native sectors.
 *   We always issue I/O in 4 KiB units (NEBULA_BLOCK_SIZE).
 *   For 512-byte devices: 1 nebula block = 8 NVMe sectors.
 *   For 4096-byte devices: 1 nebula block = 1 NVMe sector.
 */
#include "nebula_spdk_nvme.h"
#include "nebula_spdk_env.h"

#include <spdk/nvme.h>
#include <spdk/env.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NEBULA_BLOCK_SIZE 4096U

/* ---------- internal handle ------------------------------------------ */

struct nebula_spdk_ns {
    struct spdk_nvme_ctrlr *ctrlr;
    struct spdk_nvme_ns    *ns;
    struct spdk_nvme_qpair *qpair;
    uint32_t                sector_size;      /* native bytes per sector */
    uint32_t                sectors_per_block; /* NEBULA_BLOCK_SIZE / sector_size */
    uint64_t                capacity_blocks;  /* in 4 KiB units */
};

/* ---------- probe callbacks ------------------------------------------ */

/* Context passed through the probe/attach callbacks */
struct probe_ctx {
    const char            *traddr;           /* NULL = any */
    struct nebula_spdk_ns *result;           /* first usable ns found */
};

static bool probe_cb(void *cb_ctx,
                     const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts)
{
    struct probe_ctx *ctx = cb_ctx;
    (void)opts;

    if (ctx->result) return false;   /* already found one */

    if (ctx->traddr && strcmp(trid->traddr, ctx->traddr) != 0)
        return false;   /* not the requested device */

    return true;   /* claim this controller */
}

static void attach_cb(void *cb_ctx,
                      const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts)
{
    (void)trid; (void)opts;
    struct probe_ctx *ctx = cb_ctx;
    if (ctx->result) return;

    /* Walk namespaces; pick first active one */
    int num_ns = spdk_nvme_ctrlr_get_num_ns(ctrlr);
    for (int i = 1; i <= num_ns; i++) {
        struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, i);
        if (!ns || !spdk_nvme_ns_is_active(ns)) continue;

        uint32_t ss = spdk_nvme_ns_get_sector_size(ns);
        if (ss > NEBULA_BLOCK_SIZE) {
            fprintf(stderr, "[nebula_spdk] ns %d: sector_size %u > 4096, skip\n",
                    i, ss);
            continue;
        }

        struct spdk_nvme_qpair *qpair =
            spdk_nvme_ctrlr_alloc_io_qpair(ctrlr, NULL, 0);
        if (!qpair) {
            fprintf(stderr, "[nebula_spdk] failed to alloc qpair for ns %d\n", i);
            continue;
        }

        struct nebula_spdk_ns *h = calloc(1, sizeof(*h));
        if (!h) { spdk_nvme_ctrlr_free_io_qpair(qpair); continue; }

        h->ctrlr              = ctrlr;
        h->ns                 = ns;
        h->qpair              = qpair;
        h->sector_size        = ss;
        h->sectors_per_block  = NEBULA_BLOCK_SIZE / ss;
        h->capacity_blocks    = spdk_nvme_ns_get_num_sectors(ns) /
                                h->sectors_per_block;

        ctx->result = h;
        return;
    }

    fprintf(stderr, "[nebula_spdk] no usable namespace on controller\n");
}

/* ---------- completion polling --------------------------------------- */

struct io_ctx {
    bool     done;
    int      status;   /* 0 = success, -1 = error */
};

static void io_complete_cb(void *ctx,
                           const struct spdk_nvme_cpl *cpl)
{
    struct io_ctx *c = ctx;
    c->status = spdk_nvme_cpl_is_error(cpl) ? -1 : 0;
    c->done   = true;
}

static int poll_until_done(struct nebula_spdk_ns *h, struct io_ctx *ctx)
{
    while (!ctx->done)
        spdk_nvme_qpair_process_completions(h->qpair, 0);
    return ctx->status;
}

/* ---------- public API ----------------------------------------------- */

int nebula_spdk_nvme_probe(const char *traddr, struct nebula_spdk_ns **out)
{
    if (!out) return -1;

    struct probe_ctx ctx = { .traddr = traddr, .result = NULL };

    struct spdk_nvme_transport_id trid;
    memset(&trid, 0, sizeof(trid));
    trid.trtype = SPDK_NVME_TRANSPORT_PCIE;

    int rc = spdk_nvme_probe(&trid, &ctx, probe_cb, attach_cb, NULL);
    if (rc != 0) {
        fprintf(stderr, "[nebula_spdk] spdk_nvme_probe failed: %d\n", rc);
        return -1;
    }

    if (!ctx.result) {
        fprintf(stderr, "[nebula_spdk] no suitable NVMe namespace found\n");
        return -1;
    }

    *out = ctx.result;
    return 0;
}

uint64_t nebula_spdk_nvme_capacity_blocks(const struct nebula_spdk_ns *ns)
{
    return ns ? ns->capacity_blocks : 0;
}

int nebula_spdk_nvme_read(struct nebula_spdk_ns *h,
                          uint64_t lba, uint32_t n_blocks, void *buf)
{
    if (!h || !buf) return -1;

    struct io_ctx ctx = { .done = false, .status = 0 };
    uint64_t nvme_lba    = lba * h->sectors_per_block;
    uint32_t nvme_blocks = n_blocks * h->sectors_per_block;

    int rc = spdk_nvme_ns_cmd_read(h->ns, h->qpair, buf,
                                   nvme_lba, nvme_blocks,
                                   io_complete_cb, &ctx, 0);
    if (rc != 0) {
        fprintf(stderr, "[nebula_spdk] read submit failed: %d\n", rc);
        return -1;
    }
    return poll_until_done(h, &ctx);
}

int nebula_spdk_nvme_write(struct nebula_spdk_ns *h,
                           uint64_t lba, uint32_t n_blocks, const void *buf)
{
    if (!h || !buf) return -1;

    struct io_ctx ctx = { .done = false, .status = 0 };
    uint64_t nvme_lba    = lba * h->sectors_per_block;
    uint32_t nvme_blocks = n_blocks * h->sectors_per_block;

    /* SPDK write API takes non-const void* */
    int rc = spdk_nvme_ns_cmd_write(h->ns, h->qpair, (void *)buf,
                                    nvme_lba, nvme_blocks,
                                    io_complete_cb, &ctx, 0);
    if (rc != 0) {
        fprintf(stderr, "[nebula_spdk] write submit failed: %d\n", rc);
        return -1;
    }
    return poll_until_done(h, &ctx);
}

int nebula_spdk_nvme_flush(struct nebula_spdk_ns *h)
{
    if (!h) return -1;

    struct io_ctx ctx = { .done = false, .status = 0 };
    int rc = spdk_nvme_ns_cmd_flush(h->ns, h->qpair, io_complete_cb, &ctx);
    if (rc != 0) {
        fprintf(stderr, "[nebula_spdk] flush submit failed: %d\n", rc);
        return -1;
    }
    return poll_until_done(h, &ctx);
}

void nebula_spdk_nvme_close(struct nebula_spdk_ns *h)
{
    if (!h) return;
    if (h->qpair) spdk_nvme_ctrlr_free_io_qpair(h->qpair);
    if (h->ctrlr) spdk_nvme_detach(h->ctrlr);
    free(h);
}
