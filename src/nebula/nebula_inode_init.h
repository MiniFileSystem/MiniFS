/*
 * nebula_inode_init.h - Initialize inode page (root inode) and
 * directory page (entry for "/").
 */
#ifndef NEBULA_INODE_INIT_H
#define NEBULA_INODE_INIT_H

#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

int nebula_inode_page_init(struct nebula_io *io, const struct nebula_layout *L);
int nebula_dir_page_init(struct nebula_io *io, const struct nebula_layout *L);

uint32_t nebula_inode_checksum(const struct nebula_inode *ino);

#endif
