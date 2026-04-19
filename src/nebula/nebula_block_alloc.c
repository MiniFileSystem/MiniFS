/*
 * nebula_block_alloc.c - First-fit block allocator.
 *
 * See header for scope and limitations.
 */
#include "nebula_block_alloc.h"
#include "nebula_mount.h"
#include "nebula_hier_bitmap.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "../util/log.h"

#include <errno.h>
#include <stdint.h>

/* ---------- bit helpers ---------- */

static inline bool bit_get(const uint8_t *bm, uint64_t idx)
{
    return (bm[idx / 8] >> (idx % 8)) & 1u;
}

static inline void bit_set(uint8_t *bm, uint64_t idx)
{
    bm[idx / 8] |= (uint8_t)(1u << (idx % 8));
}

static inline void bit_clr(uint8_t *bm, uint64_t idx)
{
    bm[idx / 8] &= (uint8_t)~(1u << (idx % 8));
}

/* ---------- on-disk + summary sync ---------- */

/* Write the bitmap pages covering bit range [first_bit, last_bit] back
 * to disk.  Returns NEBULA_OK or -errno. */
static int flush_dirty_pages(struct nebula_mount *m,
                             uint64_t first_bit, uint64_t last_bit)
{
    uint64_t first_page = first_bit / NEBULA_BITMAP_BITS_PER_PAGE;
    uint64_t last_page  = last_bit  / NEBULA_BITMAP_BITS_PER_PAGE;
    uint32_t n_pages    = (uint32_t)(last_page - first_page + 1);

    return nebula_io_write(
        m->io,
        m->sb.bitmap_lba + first_page,
        n_pages,
        m->bitmap->l0_bits + first_page * NEBULA_BLOCK_SIZE);
}

/* Apply a delta to L1 and L2 summaries for bits [first_bit, last_bit].
 * `taken` is the absolute count of bits whose state actually changed;
 * `alloc_dir` is +1 when we just allocated (free count decreases) or
 * -1 when we just freed (free count increases). */
static void update_summaries(struct nebula_mount *m,
                             uint64_t first_bit, uint64_t last_bit,
                             int alloc_dir)
{
    uint64_t first_page = first_bit / NEBULA_BITMAP_BITS_PER_PAGE;
    uint64_t last_page  = last_bit  / NEBULA_BITMAP_BITS_PER_PAGE;

    for (uint64_t p = first_page; p <= last_page; p++) {
        uint64_t ps = p * NEBULA_BITMAP_BITS_PER_PAGE;
        uint64_t pe = ps + NEBULA_BITMAP_BITS_PER_PAGE;
        uint64_t s  = first_bit > ps ? first_bit : ps;
        uint64_t e  = (last_bit + 1) < pe ? (last_bit + 1) : pe;
        uint64_t taken = e - s;

        uint32_t group = (uint32_t)(p / NEBULA_HBM_PAGES_PER_GROUP);
        if (alloc_dir > 0) {
            m->bitmap->l1_free_counts[group] -= taken;
            m->bitmap->l2_total_free         -= taken;
        } else {
            m->bitmap->l1_free_counts[group] += taken;
            m->bitmap->l2_total_free         += taken;
        }
    }
}

/* ---------- public API ---------- */

int nebula_block_alloc(struct nebula_mount *m, uint32_t n, nebula_lba_t *out_lba)
{
    if (!m || !out_lba || n == 0) return -EINVAL;
    struct nebula_hier_bitmap *h = m->bitmap;
    if (h->l2_total_free < n) return -ENOSPC;

    uint64_t total_bits = h->num_pages * NEBULA_BITMAP_BITS_PER_PAGE;
    if (total_bits > m->sb.device_capacity_blocks) {
        total_bits = m->sb.device_capacity_blocks;
    }

    uint64_t run_start = 0;
    uint32_t run_len   = 0;

    for (uint64_t bit = 0; bit < total_bits; bit++) {
        if (!bit_get(h->l0_bits, bit)) {
            if (run_len == 0) run_start = bit;
            run_len++;
            if (run_len == n) {
                /* Found a fit.  Set bits. */
                for (uint32_t i = 0; i < n; i++) {
                    bit_set(h->l0_bits, run_start + i);
                }
                uint64_t last_bit = run_start + n - 1;

                update_summaries(m, run_start, last_bit, +1);
                int rc = flush_dirty_pages(m, run_start, last_bit);
                if (rc != NEBULA_OK) {
                    /* Roll back in-memory state on disk write failure. */
                    for (uint32_t i = 0; i < n; i++) {
                        bit_clr(h->l0_bits, run_start + i);
                    }
                    update_summaries(m, run_start, last_bit, -1);
                    return rc;
                }

                *out_lba = run_start;
                return NEBULA_OK;
            }
        } else {
            run_len = 0;
        }
    }
    return -ENOSPC;
}

int nebula_block_free(struct nebula_mount *m, nebula_lba_t lba, uint32_t n)
{
    if (!m || n == 0) return -EINVAL;
    if (lba + n > m->sb.device_capacity_blocks) return -ERANGE;

    struct nebula_hier_bitmap *h = m->bitmap;

    /* All bits must currently be set (allocated) - otherwise double-free. */
    for (uint32_t i = 0; i < n; i++) {
        if (!bit_get(h->l0_bits, lba + i)) {
            NEB_ERR("block_free: LBA %lu already free",
                    (unsigned long)(lba + i));
            return -EINVAL;
        }
    }

    for (uint32_t i = 0; i < n; i++) {
        bit_clr(h->l0_bits, lba + i);
    }
    uint64_t last_bit = lba + n - 1;
    update_summaries(m, lba, last_bit, -1);

    int rc = flush_dirty_pages(m, lba, last_bit);
    if (rc != NEBULA_OK) {
        /* Roll back. */
        for (uint32_t i = 0; i < n; i++) {
            bit_set(h->l0_bits, lba + i);
        }
        update_summaries(m, lba, last_bit, +1);
    }
    return rc;
}
