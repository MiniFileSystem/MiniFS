/*
 * nebula_spdk_env.h - SPDK environment initialisation helpers.
 *
 * Must be called once at process start before any other SPDK API.
 * Configures hugepages, DMA memory, and the SPDK reactor.
 *
 * Typical call sequence:
 *   nebula_spdk_env_init(NULL);       // default: 256 MiB hugepages, 1 core
 *   ...use SPDK...
 *   nebula_spdk_env_fini();
 */
#ifndef NEBULA_SPDK_ENV_H
#define NEBULA_SPDK_ENV_H

#include <stdint.h>
#include <stddef.h>

/*
 * SPDK environment configuration.  All fields are optional;
 * set to 0 / NULL to use the defaults.
 */
struct nebula_spdk_env_opts {
    const char *name;           /* process name shown in SPDK logs (default: "nebula") */
    size_t      mem_mb;         /* hugepage memory in MiB (default: 256) */
    int         core_mask;      /* CPU core mask as hex int (default: 0x1 = core 0) */
    const char *log_level;      /* "ERROR" | "WARN" | "INFO" | "DEBUG" (default: "WARN") */
};

/*
 * Initialise the SPDK environment.
 * opts may be NULL to use all defaults.
 * Returns 0 on success, -1 on failure (check stderr for SPDK logs).
 *
 * NOTE: hugepages must already be allocated in the OS before calling this.
 *   echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 */
int nebula_spdk_env_init(const struct nebula_spdk_env_opts *opts);

/*
 * Shut down the SPDK environment cleanly.
 * Must be called after all NVMe controllers and I/O have been released.
 */
void nebula_spdk_env_fini(void);

/*
 * Allocate a DMA-safe, hugepage-backed buffer of `size` bytes aligned to
 * `align` bytes.  Returns NULL on failure.
 * Use nebula_spdk_dma_free() to release.
 */
void *nebula_spdk_dma_alloc(size_t size, size_t align);

/*
 * Free a buffer allocated with nebula_spdk_dma_alloc().
 */
void nebula_spdk_dma_free(void *buf);

#endif /* NEBULA_SPDK_ENV_H */
