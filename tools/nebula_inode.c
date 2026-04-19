/*
 * nebula_inode - CLI test tool for the inode allocator.
 *
 * Usage:
 *   nebula_inode alloc  <device> file|dir [mode]
 *   nebula_inode read   <device> <inode_num>
 *   nebula_inode free   <device> <inode_num>
 *   nebula_inode list   <device>
 */
#include "nebula/nebula_format.h"
#include "nebula/nebula_types.h"
#include "../src/nebula/nebula_mount.h"
#include "../src/nebula/nebula_inode_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *USAGE =
    "Usage:\n"
    "  nebula_inode alloc  <device> file|dir [mode_octal]\n"
    "  nebula_inode read   <device> <inode_num>\n"
    "  nebula_inode free   <device> <inode_num>\n"
    "  nebula_inode list   <device>\n";

static void print_inode(const struct nebula_inode *ino)
{
    const char *typestr =
        ino->type == NEBULA_INODE_TYPE_FILE ? "FILE" :
        ino->type == NEBULA_INODE_TYPE_DIR  ? "DIR"  :
        ino->type == NEBULA_INODE_TYPE_FREE ? "FREE" : "UNKNOWN";

    printf("  inode_num:   %lu\n",  (unsigned long)ino->inode_num);
    printf("  type:        %s (%u)\n", typestr, ino->type);
    printf("  mode:        0%o\n",   ino->mode);
    printf("  size:        %lu bytes\n", (unsigned long)ino->size_bytes);
    printf("  alloc_size:  %lu bytes\n", (unsigned long)ino->alloc_size_bytes);
    printf("  nlink:       %u\n",    ino->nlink);
    printf("  atime_ns:    %lu\n",   (unsigned long)ino->atime_ns);
    printf("  mtime_ns:    %lu\n",   (unsigned long)ino->mtime_ns);
    printf("  ctime_ns:    %lu\n",   (unsigned long)ino->ctime_ns);
    printf("  checksum:    0x%08x\n", ino->checksum);
    printf("  extents[0]:  lba=%u off=%lu sz=0x%x\n",
           ino->extents[0].em_lba,
           (unsigned long)ino->extents[0].em_offset,
           ino->extents[0].em_size_and_flags);
}

/* ---- subcommands ---- */

static int cmd_alloc(struct nebula_mount *m, int argc, char **argv)
{
    /* argv[0] = "file"|"dir", argv[1] (opt) = mode */
    if (argc < 1) { fputs(USAGE, stderr); return 1; }

    uint32_t type;
    if (strcmp(argv[0], "file") == 0)      type = NEBULA_INODE_TYPE_FILE;
    else if (strcmp(argv[0], "dir") == 0)  type = NEBULA_INODE_TYPE_DIR;
    else { fprintf(stderr, "Unknown type '%s'\n", argv[0]); return 1; }

    uint32_t mode = (type == NEBULA_INODE_TYPE_DIR) ? 0755U : 0644U;
    if (argc >= 2) mode = (uint32_t)strtoul(argv[1], NULL, 8);

    uint64_t inum = 0;
    int rc = nebula_inode_alloc(m, type, mode, &inum);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "nebula_inode_alloc failed: %s\n", strerror(-rc));
        return 1;
    }

    struct nebula_inode ino;
    rc = nebula_inode_read(m, inum, &ino);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "alloc succeeded but read-back failed: %s\n", strerror(-rc));
        return 1;
    }

    printf("Allocated inode %lu:\n", (unsigned long)inum);
    print_inode(&ino);
    return 0;
}

static int cmd_read(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    uint64_t inum = strtoull(argv[0], NULL, 10);

    struct nebula_inode ino;
    int rc = nebula_inode_read(m, inum, &ino);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "nebula_inode_read(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }

    printf("Inode %lu:\n", (unsigned long)inum);
    print_inode(&ino);
    return 0;
}

static int cmd_free(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    uint64_t inum = strtoull(argv[0], NULL, 10);

    int rc = nebula_inode_free(m, inum);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "nebula_inode_free(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }
    printf("Inode %lu freed.\n", (unsigned long)inum);
    return 0;
}

static int cmd_list(struct nebula_mount *m)
{
    uint64_t total = m->sb.inode_page_block_count;
    printf("Inode region: LBA %lu, %lu slots\n",
           (unsigned long)m->sb.inode_page_lba,
           (unsigned long)total);

    struct nebula_inode ino;
    uint64_t used = 0;
    for (uint64_t n = 0; n < total; n++) {
        int rc = nebula_inode_read(m, n, &ino);
        if (rc != NEBULA_OK) {
            printf("  [%lu] READ ERROR: %s\n", (unsigned long)n, strerror(-rc));
            continue;
        }
        if (ino.type == NEBULA_INODE_TYPE_FREE) continue;
        used++;
        const char *t =
            ino.type == NEBULA_INODE_TYPE_FILE ? "FILE" :
            ino.type == NEBULA_INODE_TYPE_DIR  ? "DIR"  : "?";
        printf("  [%4lu]  %-4s  mode=0%o  size=%lu\n",
               (unsigned long)n, t, ino.mode,
               (unsigned long)ino.size_bytes);
    }
    printf("Used: %lu / %lu inodes\n", (unsigned long)used, (unsigned long)total);
    return 0;
}

/* ---- main ---- */

int main(int argc, char **argv)
{
    if (argc < 3) { fputs(USAGE, stderr); return 1; }

    const char *subcmd = argv[1];
    const char *device = argv[2];

    struct nebula_mount *m = NULL;
    int rc = nebula_mount_open(device, &m);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "Cannot mount '%s': %s\n", device, strerror(-rc));
        return 1;
    }

    int ret = 1;
    if (strcmp(subcmd, "alloc") == 0)
        ret = cmd_alloc(m, argc - 3, argv + 3);
    else if (strcmp(subcmd, "read") == 0)
        ret = cmd_read(m, argc - 3, argv + 3);
    else if (strcmp(subcmd, "free") == 0)
        ret = cmd_free(m, argc - 3, argv + 3);
    else if (strcmp(subcmd, "list") == 0)
        ret = cmd_list(m);
    else
        fprintf(stderr, "Unknown subcommand '%s'\n%s", subcmd, USAGE);

    nebula_mount_unmount(m);
    return ret;
}
