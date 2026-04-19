/*
 * nebula_mount.c - CLI: mount a Nebula device and print its state.
 *
 * Usage:
 *   nebula_mount --path <file> [--verbose]
 *
 * (This tool does not expose the FS; it validates that a device can be
 * mounted and reports the resulting in-memory state.)
 */
#include "nebula/nebula_types.h"
#include "../src/util/log.h"
#include "../src/nebula/nebula_mount.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void usage(const char *p)
{
    fprintf(stderr, "Usage: %s --path <file> [--verbose]\n", p);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--verbose")) nebula_log_set_level(NEBULA_LOG_DEBUG);
        else { usage(argv[0]); return 2; }
    }
    if (!path) { usage(argv[0]); return 2; }

    struct nebula_mount *m = NULL;
    int rc = nebula_mount_open(path, &m);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "mount failed: %s\n", strerror(-rc));
        return 1;
    }

    nebula_mount_print(m);
    nebula_mount_unmount(m);
    return 0;
}
