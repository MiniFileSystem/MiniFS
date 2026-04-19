/*
 * nebula_alloc.c - Test CLI for the block allocator.
 *
 * Usage:
 *   nebula_alloc --path <file> --alloc <N>
 *   nebula_alloc --path <file> --free  <LBA> --n <N>
 *   nebula_alloc --path <file> --stats
 */
#include "nebula/nebula_types.h"
#include "../src/util/log.h"
#include "../src/nebula/nebula_mount.h"
#include "../src/nebula/nebula_hier_bitmap.h"
#include "../src/nebula/nebula_block_alloc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *p)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s --path <file> --alloc <N>\n"
        "  %s --path <file> --free  <LBA> --n <N>\n"
        "  %s --path <file> --stats\n",
        p, p, p);
}

int main(int argc, char **argv)
{
    const char *path   = NULL;
    int         mode   = 0;   /* 1=alloc, 2=free, 3=stats */
    uint32_t    n      = 0;
    nebula_lba_t lba   = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--alloc") && i + 1 < argc) {
            mode = 1; n = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--free") && i + 1 < argc) {
            mode = 2; lba = (nebula_lba_t)strtoull(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--n") && i + 1 < argc) {
            n = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--stats")) {
            mode = 3;
        } else {
            usage(argv[0]); return 2;
        }
    }
    if (!path || mode == 0) { usage(argv[0]); return 2; }

    struct nebula_mount *m = NULL;
    int rc = nebula_mount_open(path, &m);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "mount: %s\n", strerror(-rc));
        return 1;
    }

    uint64_t before = nebula_hbm_total_free(m->bitmap);

    switch (mode) {
    case 1: {   /* alloc */
        nebula_lba_t got;
        rc = nebula_block_alloc(m, n, &got);
        if (rc != NEBULA_OK) {
            fprintf(stderr, "alloc: %s\n", strerror(-rc));
            break;
        }
        uint64_t after = nebula_hbm_total_free(m->bitmap);
        printf("allocated %u block(s) at LBA %lu  (free: %lu -> %lu)\n",
               n, (unsigned long)got,
               (unsigned long)before, (unsigned long)after);
        break;
    }
    case 2: {   /* free */
        if (n == 0) { fprintf(stderr, "--free requires --n\n"); rc = -EINVAL; break; }
        rc = nebula_block_free(m, lba, n);
        if (rc != NEBULA_OK) {
            fprintf(stderr, "free: %s\n", strerror(-rc));
            break;
        }
        uint64_t after = nebula_hbm_total_free(m->bitmap);
        printf("freed %u block(s) at LBA %lu  (free: %lu -> %lu)\n",
               n, (unsigned long)lba,
               (unsigned long)before, (unsigned long)after);
        break;
    }
    case 3:     /* stats */
        nebula_hbm_print_summary(m->bitmap);
        rc = NEBULA_OK;
        break;
    }

    nebula_mount_unmount(m);
    return rc == NEBULA_OK ? 0 : 1;
}
