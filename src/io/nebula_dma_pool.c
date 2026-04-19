/*
 * nebula_dma_pool.c - DMA-safe buffer pool backed by SPDK hugepage memory.
 *
 * Implements a simple free-list slab: all buffers are carved from a single
 * contiguous DMA allocation to minimise TLB pressure.
 */
#include "nebula_dma_pool.h"
#include "nebula_spdk_env.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Free-list node embedded at the start of each free buffer slot. */
struct free_node {
    struct free_node *next;
};

struct nebula_dma_pool {
    void             *dma_base;   /* single contiguous DMA allocation */
    struct free_node *free_head;  /* head of free list */
    uint32_t          n_bufs;
    size_t            buf_size;   /* aligned buffer size in bytes */
    uint32_t          available;
};

/* Round sz up to a multiple of align (must be power-of-2) */
static size_t align_up(size_t sz, size_t align)
{
    return (sz + align - 1) & ~(align - 1);
}

struct nebula_dma_pool *nebula_dma_pool_create(uint32_t n_bufs, size_t buf_size)
{
    if (n_bufs == 0 || buf_size == 0) return NULL;

    /* Each buffer must be at least sizeof(free_node) and 4 KiB-aligned */
    size_t aligned = align_up(buf_size, 4096);
    if (aligned < sizeof(struct free_node)) aligned = sizeof(struct free_node);

    size_t total = (size_t)n_bufs * aligned;

    void *base = nebula_spdk_dma_alloc(total, 4096);
    if (!base) {
        fprintf(stderr, "[dma_pool] failed to allocate %zu bytes of DMA memory\n",
                total);
        return NULL;
    }

    struct nebula_dma_pool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        nebula_spdk_dma_free(base);
        return NULL;
    }

    pool->dma_base  = base;
    pool->n_bufs    = n_bufs;
    pool->buf_size  = aligned;
    pool->available = n_bufs;
    pool->free_head = NULL;

    /* Build free list */
    uint8_t *ptr = (uint8_t *)base;
    for (uint32_t i = 0; i < n_bufs; i++) {
        struct free_node *node = (struct free_node *)ptr;
        node->next      = pool->free_head;
        pool->free_head = node;
        ptr += aligned;
    }

    return pool;
}

void nebula_dma_pool_destroy(struct nebula_dma_pool *pool)
{
    if (!pool) return;
    if (pool->available != pool->n_bufs)
        fprintf(stderr, "[dma_pool] warning: %u buffer(s) still borrowed at destroy\n",
                pool->n_bufs - pool->available);
    nebula_spdk_dma_free(pool->dma_base);
    free(pool);
}

void *nebula_dma_pool_get(struct nebula_dma_pool *pool)
{
    /* Spin until a buffer is available (single-threaded: should never spin) */
    while (!pool->free_head) {
        /* In a multi-threaded environment replace with a condition variable */
    }

    struct free_node *node = pool->free_head;
    pool->free_head = node->next;
    pool->available--;

    /* Zero the buffer before handing it out */
    memset(node, 0, pool->buf_size);
    return (void *)node;
}

void nebula_dma_pool_put(struct nebula_dma_pool *pool, void *buf)
{
    if (!pool || !buf) return;
    struct free_node *node = (struct free_node *)buf;
    node->next      = pool->free_head;
    pool->free_head = node;
    pool->available++;
}

uint32_t nebula_dma_pool_available(const struct nebula_dma_pool *pool)
{
    return pool ? pool->available : 0;
}
