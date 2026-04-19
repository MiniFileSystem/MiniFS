/*
 * nebula_rw - CLI test tool for file and directory operations.
 *
 * Usage:
 *   nebula_rw create  <device> <name> [mode_octal]
 *   nebula_rw write   <device> <inode_num> <text>
 *   nebula_rw read    <device> <inode_num>
 *   nebula_rw delete  <device> <inode_num>
 *   nebula_rw diradd  <device> <name> <inode_num> file|dir
 *   nebula_rw dirlook <device> <name>
 *   nebula_rw dirrm   <device> <name>
 *   nebula_rw dirlist <device>
 */
#include "nebula/nebula_format.h"
#include "nebula/nebula_types.h"
#include "../src/nebula/nebula_mount.h"
#include "../src/nebula/nebula_file.h"
#include "../src/nebula/nebula_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static const char *USAGE =
    "Usage:\n"
    "  nebula_rw create  <dev> <name> [mode]\n"
    "  nebula_rw write   <dev> <inode_num> <text>\n"
    "  nebula_rw read    <dev> <inode_num>\n"
    "  nebula_rw delete  <dev> <inode_num>\n"
    "  nebula_rw diradd  <dev> <name> <inode_num> file|dir\n"
    "  nebula_rw dirlook <dev> <name>\n"
    "  nebula_rw dirrm   <dev> <name>\n"
    "  nebula_rw dirlist <dev>\n";

/* ---- file subcommands ---- */

static int cmd_create(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    const char *name = argv[0];
    uint32_t mode = argc >= 2 ? (uint32_t)strtoul(argv[1], NULL, 8) : 0644U;

    struct nebula_file *f = NULL;
    int rc = nebula_file_create(m, mode, &f);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "file_create failed: %s\n", strerror(-rc));
        return 1;
    }
    uint64_t inum = f->inode.inode_num;
    nebula_file_close(f);

    /* Register in directory */
    rc = nebula_dir_add(m, name, inum, NEBULA_DIR_FLAG_FILE);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "dir_add failed: %s\n", strerror(-rc));
        return 1;
    }

    printf("Created file '%s' -> inode %lu (mode=0%o)\n",
           name, (unsigned long)inum, mode);
    return 0;
}

static int cmd_write(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 2) { fputs(USAGE, stderr); return 1; }
    uint64_t inum = strtoull(argv[0], NULL, 10);
    const char *text = argv[1];

    struct nebula_file *f = NULL;
    int rc = nebula_file_open(m, inum, NEBULA_O_RDWR, &f);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "file_open(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }

    ssize_t n = nebula_file_write(f, 0, text, strlen(text));
    nebula_file_close(f);

    if (n < 0) {
        fprintf(stderr, "file_write failed: %s\n", strerror((int)-n));
        return 1;
    }
    printf("Wrote %zd bytes to inode %lu\n", n, (unsigned long)inum);
    return 0;
}

static int cmd_read(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    uint64_t inum = strtoull(argv[0], NULL, 10);

    struct nebula_file *f = NULL;
    int rc = nebula_file_open(m, inum, NEBULA_O_RDONLY, &f);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "file_open(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }

    uint64_t sz = f->inode.size_bytes;
    printf("inode %lu: size=%lu bytes\n", (unsigned long)inum, (unsigned long)sz);

    if (sz > 0) {
        char *buf = malloc(sz + 1);
        if (!buf) { nebula_file_close(f); return 1; }
        ssize_t n = nebula_file_read(f, 0, buf, (size_t)sz);
        if (n >= 0) {
            buf[n] = '\0';
            printf("Content: %s\n", buf);
        } else {
            fprintf(stderr, "file_read failed: %s\n", strerror((int)-n));
        }
        free(buf);
    }

    nebula_file_close(f);
    return 0;
}

static int cmd_delete(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    uint64_t inum = strtoull(argv[0], NULL, 10);

    struct nebula_file *f = NULL;
    int rc = nebula_file_open(m, inum, NEBULA_O_RDWR, &f);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "file_open(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }

    rc = nebula_file_delete(f);   /* frees f */
    if (rc != NEBULA_OK) {
        fprintf(stderr, "file_delete(%lu) failed: %s\n",
                (unsigned long)inum, strerror(-rc));
        return 1;
    }
    printf("Deleted inode %lu\n", (unsigned long)inum);
    return 0;
}

/* ---- directory subcommands ---- */

static int cmd_diradd(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 3) { fputs(USAGE, stderr); return 1; }
    const char *name  = argv[0];
    uint64_t inum     = strtoull(argv[1], NULL, 10);
    uint16_t tflag    = strcmp(argv[2], "dir") == 0
                        ? NEBULA_DIR_FLAG_DIR : NEBULA_DIR_FLAG_FILE;

    int rc = nebula_dir_add(m, name, inum, tflag);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "dir_add('%s') failed: %s\n", name, strerror(-rc));
        return 1;
    }
    printf("Added dir entry '%s' -> inode %lu\n", name, (unsigned long)inum);
    return 0;
}

static int cmd_dirlook(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    const char *name = argv[0];
    uint64_t inum = 0;

    int rc = nebula_dir_lookup(m, name, &inum);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "dir_lookup('%s') failed: %s\n", name, strerror(-rc));
        return 1;
    }
    printf("'%s' -> inode %lu\n", name, (unsigned long)inum);
    return 0;
}

static int cmd_dirrm(struct nebula_mount *m, int argc, char **argv)
{
    if (argc < 1) { fputs(USAGE, stderr); return 1; }
    const char *name = argv[0];

    int rc = nebula_dir_remove(m, name);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "dir_remove('%s') failed: %s\n", name, strerror(-rc));
        return 1;
    }
    printf("Removed dir entry '%s'\n", name);
    return 0;
}

static int list_cb(const char *name, uint64_t inode_num,
                   uint16_t flags, void *ud)
{
    (void)ud;
    const char *t = (flags & NEBULA_DIR_FLAG_DIR) ? "DIR" : "FILE";
    printf("  %-4s  inode=%-6lu  %s\n", t, (unsigned long)inode_num, name);
    return 0;
}

static int cmd_dirlist(struct nebula_mount *m)
{
    printf("Directory listing:\n");
    int rc = nebula_dir_list(m, list_cb, NULL);
    if (rc != NEBULA_OK)
        fprintf(stderr, "dir_list failed: %s\n", strerror(-rc));
    return rc == NEBULA_OK ? 0 : 1;
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
    if      (strcmp(subcmd, "create")  == 0) ret = cmd_create(m,  argc-3, argv+3);
    else if (strcmp(subcmd, "write")   == 0) ret = cmd_write(m,   argc-3, argv+3);
    else if (strcmp(subcmd, "read")    == 0) ret = cmd_read(m,    argc-3, argv+3);
    else if (strcmp(subcmd, "delete")  == 0) ret = cmd_delete(m,  argc-3, argv+3);
    else if (strcmp(subcmd, "diradd")  == 0) ret = cmd_diradd(m,  argc-3, argv+3);
    else if (strcmp(subcmd, "dirlook") == 0) ret = cmd_dirlook(m, argc-3, argv+3);
    else if (strcmp(subcmd, "dirrm")   == 0) ret = cmd_dirrm(m,  argc-3, argv+3);
    else if (strcmp(subcmd, "dirlist") == 0) ret = cmd_dirlist(m);
    else fprintf(stderr, "Unknown subcommand '%s'\n%s", subcmd, USAGE);

    nebula_mount_unmount(m);
    return ret;
}
