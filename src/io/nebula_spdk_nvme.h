/*
 * nebula_spdk_nvme.h - NVMe controller probe, namespace and I/O qpair.
 *
 * Usage:
 *   struct nebula_spdk_ns *ns = NULL;
 *   nebula_spdk_nvme_probe(NULL, &ns);   // probe all local NVMe, pick first ns
 *   ...do I/O via nebula_spdk_nvme_read / _write...
 *   nebula_spdk_nvme_close(ns);
 */
#ifndef NEBULA_SPDK_NVME_H
#define NEBULA_SPDK_NVME_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Opaque handle encapsulating controller + namespace + qpair. */
struct nebula_spdk_ns;

/*
 * Probe for NVMe controllers over PCIe.
 * traddr: PCIe BDF string e.g. "0000:01:00.0".
 *         Pass NULL to probe all available local NVMe devices.
 * out:    receives the first usable namespace handle on success.
 * Returns 0 on success, -1 on failure.
 */
int nebula_spdk_nvme_probe(const char *traddr, struct nebula_spdk_ns **out);

/*
 * Probe using a fully populated spdk_nvme_transport_id.
 * Supports PCIe, TCP, RDMA — caller fills in trtype/traddr/trsvcid/subnqn.
 * out: receives the namespace handle on success.
 * Returns 0 on success, -1 on failure.
 */
struct spdk_nvme_transport_id;   /* forward-declare — avoid pulling spdk/nvme.h here */
int nebula_spdk_nvme_probe_trid(const struct spdk_nvme_transport_id *trid,
                                struct nebula_spdk_ns **out);

/*
 * Return the namespace capacity in 4 KiB logical blocks.
 * (Converts from the device's native sector size automatically.)
 */
uint64_t nebula_spdk_nvme_capacity_blocks(const struct nebula_spdk_ns *ns);

/*
 * Synchronous read of n_blocks × 4 KiB starting at lba into buf.
 * buf must be DMA-safe (allocated via nebula_spdk_dma_alloc).
 * Returns 0 on success, -1 on I/O error.
 */
int nebula_spdk_nvme_read(struct nebula_spdk_ns *ns,
                          uint64_t lba, uint32_t n_blocks, void *buf);

/*
 * Synchronous write of n_blocks × 4 KiB from buf starting at lba.
 * buf must be DMA-safe.
 * Returns 0 on success, -1 on I/O error.
 */
int nebula_spdk_nvme_write(struct nebula_spdk_ns *ns,
                           uint64_t lba, uint32_t n_blocks, const void *buf);

/*
 * Flush (FUA / NVMe flush command) to ensure data is persistent.
 * Returns 0 on success, -1 on error.
 */
int nebula_spdk_nvme_flush(struct nebula_spdk_ns *ns);

/*
 * Release qpair, detach controller, free the handle.
 */
void nebula_spdk_nvme_close(struct nebula_spdk_ns *ns);

#endif /* NEBULA_SPDK_NVME_H */
