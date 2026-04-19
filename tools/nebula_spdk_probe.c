/*
 * nebula_spdk_probe - Discover NVMe devices and verify the SPDK backend.
 *
 * Usage:
 *   nebula_spdk_probe                      # probe all NVMe devices
 *   nebula_spdk_probe <pcie_traddr>        # probe specific device
 *   nebula_spdk_probe <pcie_traddr> mount  # format-check as Nebula device
 *
 * Prerequisites:
 *   - Hugepages allocated:
 *       echo 512 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
 *   - NVMe device unbound from kernel and bound to vfio-pci:
 *       sudo ./spdk/scripts/setup.sh
 */
#include "../src/io/nebula_spdk_env.h"
#include "../src/io/nebula_spdk_nvme.h"
#include "../src/io/nebula_io_spdk.h"
#include "../src/nebula/nebula_mount.h"
#include "nebula/nebula_io.h"
#include "nebula/nebula_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_device_info(struct nebula_spdk_ns *ns)
{
    uint64_t cap   = nebula_spdk_nvme_capacity_blocks(ns);
    double   cap_g = (double)cap * NEBULA_BLOCK_SIZE / (1024.0 * 1024.0 * 1024.0);
    printf("  Capacity : %lu blocks (%.2f GiB)\n",
           (unsigned long)cap, cap_g);
}

static int do_probe(const char *traddr)
{
    printf("Probing NVMe device: %s\n",
           traddr ? traddr : "(all local devices)");

    struct nebula_spdk_ns *ns = NULL;
    if (nebula_spdk_nvme_probe(traddr, &ns) != 0) {
        fprintf(stderr, "No NVMe namespace found.\n");
        return 1;
    }

    printf("Found namespace:\n");
    print_device_info(ns);

    /* Quick read/write self-test: write a known pattern to block 0 and
     * read it back (only if device is NOT already formatted as Nebula). */
    printf("Running I/O self-test on block 0...\n");

    void *wbuf = nebula_spdk_dma_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    void *rbuf = nebula_spdk_dma_alloc(NEBULA_BLOCK_SIZE, NEBULA_BLOCK_SIZE);
    if (!wbuf || !rbuf) {
        fprintf(stderr, "DMA alloc failed\n");
        nebula_spdk_nvme_close(ns);
        return 1;
    }

    memset(wbuf, 0xA5, NEBULA_BLOCK_SIZE);
    int rc = nebula_spdk_nvme_write(ns, 0, 1, wbuf);
    if (rc != 0) { fprintf(stderr, "Write failed\n"); goto fail; }

    rc = nebula_spdk_nvme_read(ns, 0, 1, rbuf);
    if (rc != 0) { fprintf(stderr, "Read failed\n"); goto fail; }

    if (memcmp(wbuf, rbuf, NEBULA_BLOCK_SIZE) != 0) {
        fprintf(stderr, "Read-back mismatch!\n"); goto fail;
    }
    printf("I/O self-test: PASS\n");

    nebula_spdk_dma_free(wbuf);
    nebula_spdk_dma_free(rbuf);
    nebula_spdk_nvme_close(ns);
    return 0;

fail:
    nebula_spdk_dma_free(wbuf);
    nebula_spdk_dma_free(rbuf);
    nebula_spdk_nvme_close(ns);
    return 1;
}

static int do_mount_check(const char *traddr)
{
    printf("Attempting Nebula mount via SPDK on %s...\n", traddr);

    struct nebula_io *io = NULL;
    int rc = nebula_io_spdk_open(traddr, &io);
    if (rc != NEBULA_OK) {
        fprintf(stderr, "nebula_io_spdk_open failed: %d\n", rc);
        return 1;
    }

    printf("  Capacity: %lu blocks (%.2f GiB)\n",
           (unsigned long)nebula_io_capacity_blocks(io),
           (double)nebula_io_capacity_blocks(io) *
           NEBULA_BLOCK_SIZE / (1024.0 * 1024.0 * 1024.0));

    nebula_io_close(io);
    printf("SPDK I/O backend opened successfully.\n");
    printf("Run nebula_format --path <traddr> to format the device.\n");
    return 0;
}

int main(int argc, char **argv)
{
    const char *traddr    = argc >= 2 ? argv[1] : NULL;
    bool        do_mount  = argc >= 3 && strcmp(argv[2], "mount") == 0;

    /* Initialise SPDK environment */
    struct nebula_spdk_env_opts opts = {
        .name      = "nebula_spdk_probe",
        .mem_mb    = 256,
        .log_level = "WARN",
    };
    if (nebula_spdk_env_init(&opts) != 0) {
        fprintf(stderr, "SPDK env init failed.\n"
                "Ensure hugepages are allocated and device is bound to vfio-pci.\n");
        return 1;
    }

    int ret = do_mount ? do_mount_check(traddr) : do_probe(traddr);

    nebula_spdk_env_fini();
    return ret;
}
