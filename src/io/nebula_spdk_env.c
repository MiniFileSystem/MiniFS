/*
 * nebula_spdk_env.c - SPDK environment initialisation.
 *
 * Compiled only when NEBULA_ENABLE_SPDK=ON (CMake guard).
 * Wraps spdk_env_init / spdk_env_dpdk_post_init and the DMA allocator.
 */
#include "nebula_spdk_env.h"

#include <spdk/env.h>
#include <spdk/log.h>

#include <stdio.h>
#include <string.h>

int nebula_spdk_env_init(const struct nebula_spdk_env_opts *opts)
{
    struct spdk_env_opts spdk_opts;
    spdk_env_opts_init(&spdk_opts);

    /* Apply caller overrides */
    if (opts) {
        if (opts->name)      spdk_opts.name      = opts->name;
        if (opts->mem_mb)    spdk_opts.mem_size   = (int)opts->mem_mb;
        if (opts->core_mask) spdk_opts.core_mask  = NULL; /* set via shm_id path */
    }

    /* Defaults */
    if (!spdk_opts.name)    spdk_opts.name     = "nebula";
    if (!spdk_opts.mem_size) spdk_opts.mem_size = 256; /* MiB */

    /* Set log level before init */
    const char *lvl = (opts && opts->log_level) ? opts->log_level : "WARN";
    if      (strcmp(lvl, "ERROR") == 0) spdk_log_set_level(SPDK_LOG_ERROR);
    else if (strcmp(lvl, "INFO")  == 0) spdk_log_set_level(SPDK_LOG_INFO);
    else if (strcmp(lvl, "DEBUG") == 0) spdk_log_set_level(SPDK_LOG_DEBUG);
    else                                spdk_log_set_level(SPDK_LOG_WARN);

    if (spdk_env_init(&spdk_opts) < 0) {
        fprintf(stderr, "[nebula_spdk] spdk_env_init failed. "
                "Check hugepages: echo 512 > "
                "/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages\n");
        return -1;
    }

    return 0;
}

void nebula_spdk_env_fini(void)
{
    spdk_env_fini();
}

void *nebula_spdk_dma_alloc(size_t size, size_t align)
{
    return spdk_dma_zmalloc(size, align, NULL);
}

void nebula_spdk_dma_free(void *buf)
{
    spdk_dma_free(buf);
}
