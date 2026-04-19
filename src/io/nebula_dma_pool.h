/*
 * nebula_dma_pool.h - DMA-safe buffer pool for SPDK I/O.
 *
 * Pre-allocates a fixed number of 4 KiB-aligned DMA buffers from hugepage
 * memory and hands them out / reclaims them in O(1).
 *
 * Intended use: callers borrow a buffer, submit SPDK I/O, then return it.
 * The pool is not thread-safe; external serialisation is required.
 *
 * Usage:
 *   struct nebula_dma_pool *pool = nebula_dma_pool_create(64, 4096);
 *   void *buf = nebula_dma_pool_get(pool);
 *   // ... do I/O ...
 *   nebula_dma_pool_put(pool, buf);
 *   nebula_dma_pool_destroy(pool);
 */
#ifndef NEBULA_DMA_POOL_H
#define NEBULA_DMA_POOL_H

#include <stddef.h>
#include <stdint.h>

struct nebula_dma_pool;

/*
 * Create a pool of `n_bufs` DMA-safe buffers each of `buf_size` bytes.
 * buf_size will be rounded up to a multiple of 4096.
 * Returns NULL on allocation failure.
 */
struct nebula_dma_pool *nebula_dma_pool_create(uint32_t n_bufs,
                                               size_t   buf_size);

/*
 * Destroy the pool and free all DMA memory.
 * All buffers must have been returned via nebula_dma_pool_put() first.
 */
void nebula_dma_pool_destroy(struct nebula_dma_pool *pool);

/*
 * Borrow a buffer from the pool.
 * Blocks (spins) if no buffer is currently available.
 * Returns a pointer to a zeroed DMA-safe buffer.
 */
void *nebula_dma_pool_get(struct nebula_dma_pool *pool);

/*
 * Return a buffer to the pool.  buf must have been obtained from
 * nebula_dma_pool_get() on the same pool instance.
 */
void nebula_dma_pool_put(struct nebula_dma_pool *pool, void *buf);

/*
 * Return number of currently available (not borrowed) buffers.
 */
uint32_t nebula_dma_pool_available(const struct nebula_dma_pool *pool);

#endif /* NEBULA_DMA_POOL_H */
