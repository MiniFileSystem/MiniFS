/*
 * nebula_uberblock.h - Uberblock region initialization.
 */
#ifndef NEBULA_UB_PRIV_H
#define NEBULA_UB_PRIV_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

/* Initialize the uberblock region: zero all 128 slots, then populate slot 0
 * with a valid TXG=0 uberblock pointing at the initial bitmap/stream map. */
int nebula_uberblock_init_region(struct nebula_io *io,
                                 const struct nebula_layout *L);

int nebula_uberblock_read_slot(struct nebula_io *io, const struct nebula_layout *L,
                               uint32_t slot, struct nebula_uberblock *out);

uint32_t nebula_uberblock_checksum(const struct nebula_uberblock *ub);

#endif
