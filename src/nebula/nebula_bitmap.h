/*
 * nebula_bitmap.h - Zero-initialize bitmap region.
 */
#ifndef NEBULA_BM_PRIV_H
#define NEBULA_BM_PRIV_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

/* Write all-zero bitmap pages (all blocks free). Also zeroes the stream map
 * region so it starts empty. */
int nebula_bitmap_init(struct nebula_io *io, const struct nebula_layout *L);

#endif
