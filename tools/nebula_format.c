/*
 * nebula_format.c - mkfs for Nebula FS.
 *
 * Usage:
 *   nebula_format --path <file> [--size <bytes>] [--force]
 *
 * If --size is given and the file does not exist (or --force is given
 * with a new size), the file is created/truncated to that size first.
 */
#include "nebula/nebula_types.h"
#include "nebula/nebula_format.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_layout.h"

#ifdef NEBULA_ENABLE_SPDK
#include "../src/io/nebula_spdk_env.h"
#include "../src/io/nebula_io_spdk.h"
#endif

#include "../src/util/uuid.h"
#include "../src/util/log.h"

#include "../src/nebula/nebula_mbr.h"
#include "../src/nebula/nebula_superblock.h"
#include "../src/nebula/nebula_uberblock.h"
#include "../src/nebula/nebula_allocator_roots.h"
#include "../src/nebula/nebula_bitmap.h"
#include "../src/nebula/nebula_inode_init.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --path <file|pci-bdf> [--size <bytes|NK|NM|NG>]\n"
        "           [--inode-size 4096|8192] [--force] [--verbose]\n"
        "           [--spdk]  (use SPDK NVMe backend; path = PCIe BDF)\n"
        "  --path         Path to device/image OR PCIe BDF (e.g. 0000:00:02.0)\n"
        "  --size         Size if creating a new image file (e.g. 1G, 512M)\n"
        "                 Ignored for real NVMe devices (use 0 or omit)\n"
        "  --inode-size   Inode size in bytes: 4096 (default) or 8192\n"
        "  --spdk         Use SPDK NVMe backend (requires hugepages + vfio-pci)\n"
        "  --force        Overwrite an existing Nebula image without asking\n"
        "  --verbose      Enable debug logging\n",
        prog);
}

static int parse_size(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (!end || end == s) return -EINVAL;
    uint64_t mul = 1;
    if (*end) {
        switch (*end) {
        case 'k': case 'K': mul = 1024ULL; break;
        case 'm': case 'M': mul = 1024ULL * 1024; break;
        case 'g': case 'G': mul = 1024ULL * 1024 * 1024; break;
        case 't': case 'T': mul = 1024ULL * 1024 * 1024 * 1024; break;
        default: return -EINVAL;
        }
    }
    *out = (uint64_t)v * mul;
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    uint64_t    size = 0;
    uint32_t    inode_size = NEBULA_INODE_SIZE_DEFAULT;
    bool        force    = false;
    bool        use_spdk = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--path") && i + 1 < argc)      path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (parse_size(argv[++i], &size) < 0) {
                fprintf(stderr, "invalid --size\n"); return 2;
            }
        }
        else if (!strcmp(argv[i], "--inode-size") && i + 1 < argc) {
            unsigned long v = strtoul(argv[++i], NULL, 10);
            if (v != NEBULA_INODE_SIZE_DEFAULT && v != NEBULA_INODE_SIZE_LARGE) {
                fprintf(stderr,
                    "invalid --inode-size %lu (must be 4096 or 8192)\n", v);
                return 2;
            }
            inode_size = (uint32_t)v;
        }
        else if (!strcmp(argv[i], "--force"))   force = true;
        else if (!strcmp(argv[i], "--spdk"))    use_spdk = true;
        else if (!strcmp(argv[i], "--verbose")) nebula_log_set_level(NEBULA_LOG_DEBUG);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]); return 0;
        }
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    if (!path) { usage(argv[0]); return 2; }
    (void)force; /* reserved */

    if (use_spdk) {
#ifndef NEBULA_ENABLE_SPDK
        fprintf(stderr, "Error: --spdk requires building with -DNEBULA_ENABLE_SPDK=ON\n");
        return 1;
#else
        NEB_INFO("Initialising SPDK environment...");
        if (nebula_spdk_env_init(NULL) != NEBULA_OK) {
            fprintf(stderr, "SPDK env init failed\n"); return 1;
        }
#endif
    }

    struct nebula_io *io = NULL;
    int rc;

    if (use_spdk) {
#ifdef NEBULA_ENABLE_SPDK
        rc = nebula_io_spdk_open(path, &io);
        if (rc != NEBULA_OK) {
            fprintf(stderr, "SPDK open %s: %s\n", path, strerror(-rc));
            nebula_spdk_env_fini(); return 1;
        }
#endif
    } else {
        rc = nebula_io_open(path, /*create=*/size > 0, size, &io);
        if (rc != NEBULA_OK) {
            fprintf(stderr, "Failed to open %s: %s\n", path, strerror(-rc));
            return 1;
        }
    }

    uint64_t capacity_blocks = nebula_io_capacity_blocks(io);
    NEB_INFO("Device: %s", path);
    NEB_INFO("Capacity: %lu blocks (%.2f MiB)",
             (unsigned long)capacity_blocks,
             (double)capacity_blocks * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0));

    struct nebula_layout L;
    rc = nebula_layout_compute_ex(capacity_blocks, inode_size, &L);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "Layout compute failed: device too small (min %lu MiB)\n",
                (unsigned long)(NEBULA_MIN_DEVICE_BYTES / (1024 * 1024)));
        nebula_io_close(io);
        return 1;
    }
    nebula_layout_print(&L);

    uint8_t uuid[16];
    if ((rc = nebula_uuid_generate(uuid)) != 0) {
        fprintf(stderr, "UUID gen failed: %s\n", strerror(-rc));
        nebula_io_close(io); return 1;
    }

    char us[NEBULA_UUID_STR_LEN + 1];
    nebula_uuid_format(uuid, us);
    NEB_INFO("Device UUID: %s", us);

    /* --- Write everything --- */
    NEB_INFO("Writing MBR...");
    if ((rc = nebula_mbr_write(io, uuid, capacity_blocks)) != NEBULA_OK) goto fail;

    NEB_INFO("Writing uberblock region (128 slots)...");
    if ((rc = nebula_uberblock_init_region(io, &L)) != NEBULA_OK) goto fail;

    NEB_INFO("Writing allocator roots (64 head + 64 tail)...");
    if ((rc = nebula_allocator_roots_init(io, &L)) != NEBULA_OK) goto fail;

    NEB_INFO("Initializing bitmap (%lu pages) + stream map...",
             (unsigned long)L.bitmap_block_count);
    if ((rc = nebula_bitmap_init(io, &L)) != NEBULA_OK) goto fail;

    NEB_INFO("Initializing inode page (%lu MiB) with root inode...",
             (unsigned long)(L.inode_page_block_count * NEBULA_BLOCK_SIZE / (1024*1024)));
    if ((rc = nebula_inode_page_init(io, &L)) != NEBULA_OK) goto fail;

    NEB_INFO("Initializing directory page with '/' entry...");
    if ((rc = nebula_dir_page_init(io, &L)) != NEBULA_OK) goto fail;

    /* Superblocks LAST so device is only "valid" once everything else exists. */
    NEB_INFO("Writing superblock (head + tail)...");
    if ((rc = nebula_superblock_write_both(io, uuid, &L)) != NEBULA_OK) goto fail;

    NEB_INFO("Flushing...");
    if ((rc = nebula_io_flush(io)) != NEBULA_OK) goto fail;

    nebula_io_close(io);
    NEB_INFO("Done. Device formatted successfully.");
#ifdef NEBULA_ENABLE_SPDK
    if (use_spdk) nebula_spdk_env_fini();
#endif
    return 0;

fail:
    fprintf(stderr, "Format failed: %s\n", strerror(-rc));
    nebula_io_close(io);
#ifdef NEBULA_ENABLE_SPDK
    if (use_spdk) nebula_spdk_env_fini();
#endif
    return 1;
}
