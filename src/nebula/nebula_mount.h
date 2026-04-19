/*
 * nebula_mount.h - Mount a Nebula device into an in-memory state.
 */
#ifndef NEBULA_MOUNT_H
#define NEBULA_MOUNT_H

#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"

struct nebula_hier_bitmap;

struct nebula_mount {
    struct nebula_io         *io;
    struct nebula_mbr         mbr;
    struct nebula_superblock  sb;
    struct nebula_uberblock   current_ub;
    uint32_t                  current_ub_slot;
    struct nebula_hier_bitmap *bitmap;
};

/* Open and mount a device. Returns NEBULA_OK or -errno.
 * On success, caller must free with nebula_mount_unmount().
 */
int nebula_mount_open(const char *path, struct nebula_mount **out);

void nebula_mount_unmount(struct nebula_mount *m);

/* Pretty-print mount state for `nebula_mount` CLI. */
void nebula_mount_print(const struct nebula_mount *m);

#endif
