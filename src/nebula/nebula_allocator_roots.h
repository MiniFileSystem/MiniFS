/*
 * nebula_allocator_roots.h - 128 allocator roots (64 head + 64 tail).
 */
#ifndef NEBULA_AR_PRIV_H
#define NEBULA_AR_PRIV_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

/* Partition the data region among 128 roots (2 blocks: head + tail).
 * Each root covers an equal slice of the data region. */
int nebula_allocator_roots_init(struct nebula_io *io,
                                const struct nebula_layout *L);

int nebula_allocator_roots_read(struct nebula_io *io, nebula_lba_t lba,
                                struct nebula_allocator_root roots[NEBULA_ALLOC_ROOTS_HEAD]);

uint32_t nebula_allocator_root_checksum(const struct nebula_allocator_root *r);

#endif
