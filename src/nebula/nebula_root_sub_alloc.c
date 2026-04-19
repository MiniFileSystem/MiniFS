/*
 * nebula_root_sub_alloc.c - 32 KiB directory slot allocator over a
 * 128 MiB root-inode chunk. See nebula_root_sub_alloc.h.
 *
 * Bit layout (bm->bits[] is 4096 bytes = 32768 bits):
 *   bit i == 1 -> 4 KiB sub-block i is allocated
 *   bit i == 0 -> free
 * Directory slot N occupies sub-blocks [N*8 .. N*8+7] (8 bits).
 * Sub-blocks 0..7 are reserved for the bitmap itself (slot 0 is never
 * returned by the allocator).
 */
#include "nebula_root_sub_alloc.h"

#include <errno.h>
#include <string.h>

static inline bool bit_get(const uint8_t *bits, uint32_t i)
{
    return (bits[i >> 3] >> (i & 7)) & 1u;
}

static inline void bit_set(uint8_t *bits, uint32_t i)
{
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
}

static inline void bit_clear(uint8_t *bits, uint32_t i)
{
    bits[i >> 3] &= (uint8_t)~(1u << (i & 7));
}

/* A slot covers NEBULA_ROOT_DIR_SLOT_SUBBLOCKS (=8) consecutive sub-blocks
 * starting at (slot * 8). */
static inline uint32_t slot_base_subblock(uint32_t slot)
{
    return slot * NEBULA_ROOT_DIR_SLOT_SUBBLOCKS;
}

void nebula_root_chunk_bitmap_init(struct nebula_root_chunk_bitmap *bm)
{
    if (!bm) return;
    memset(bm->bits, 0, sizeof(bm->bits));
    /* Reserve slot 0 (sub-blocks 0..7) for the bitmap page itself. */
    for (uint32_t i = 0; i < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; i++) {
        bit_set(bm->bits, i);
    }
}

int nebula_root_chunk_alloc_dir(struct nebula_root_chunk_bitmap *bm,
                                uint32_t *out_slot)
{
    if (!bm || !out_slot) return -EINVAL;

    /* Slot 0 is reserved; scan slots 1..SLOTS_PER_CHUNK-1. */
    for (uint32_t slot = 1; slot < NEBULA_ROOT_DIR_SLOTS_PER_CHUNK; slot++) {
        uint32_t base = slot_base_subblock(slot);
        bool     free = true;
        for (uint32_t j = 0; j < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; j++) {
            if (bit_get(bm->bits, base + j)) { free = false; break; }
        }
        if (!free) continue;

        for (uint32_t j = 0; j < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; j++) {
            bit_set(bm->bits, base + j);
        }
        *out_slot = slot;
        return NEBULA_OK;
    }
    return -ENOSPC;
}

int nebula_root_chunk_free_dir(struct nebula_root_chunk_bitmap *bm,
                               uint32_t slot)
{
    if (!bm) return -EINVAL;
    if (slot == 0 || slot >= NEBULA_ROOT_DIR_SLOTS_PER_CHUNK) return -EINVAL;

    uint32_t base = slot_base_subblock(slot);
    for (uint32_t j = 0; j < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; j++) {
        if (!bit_get(bm->bits, base + j)) return -EINVAL;  /* double-free */
    }
    for (uint32_t j = 0; j < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; j++) {
        bit_clear(bm->bits, base + j);
    }
    return NEBULA_OK;
}

uint32_t nebula_root_chunk_free_slots(const struct nebula_root_chunk_bitmap *bm)
{
    if (!bm) return 0;
    uint32_t free_cnt = 0;
    for (uint32_t slot = 1; slot < NEBULA_ROOT_DIR_SLOTS_PER_CHUNK; slot++) {
        uint32_t base = slot_base_subblock(slot);
        bool     free = true;
        for (uint32_t j = 0; j < NEBULA_ROOT_DIR_SLOT_SUBBLOCKS; j++) {
            if (bit_get(bm->bits, base + j)) { free = false; break; }
        }
        if (free) free_cnt++;
    }
    return free_cnt;
}
