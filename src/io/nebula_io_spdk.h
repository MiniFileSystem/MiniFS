/*
 * nebula_io_spdk.h - SPDK NVMe I/O backend factory.
 *
 * Creates a nebula_io handle backed by an NVMe namespace instead of
 * a POSIX file.  Drop-in replacement for nebula_io_open() when SPDK
 * is available.
 *
 * Usage:
 *   nebula_spdk_env_init(NULL);
 *   struct nebula_io *io = NULL;
 *   nebula_io_spdk_open("0000:01:00.0", &io);
 *   // use io with all normal nebula_io_* APIs
 *   nebula_io_close(io);
 *   nebula_spdk_env_fini();
 */
#ifndef NEBULA_IO_SPDK_H
#define NEBULA_IO_SPDK_H

#include "nebula/nebula_io.h"

/*
 * Open an NVMe device as a nebula_io backend.
 * traddr: PCIe BDF string e.g. "0000:01:00.0", or NULL to use the first
 *         available NVMe namespace.
 * out:    receives the nebula_io handle on success.
 * Returns NEBULA_OK or -errno equivalent.
 *
 * Prerequisites:
 *   - nebula_spdk_env_init() must have been called.
 *   - The target NVMe device must be unbound from the kernel driver
 *     and bound to vfio-pci or uio_pci_generic.
 */
int nebula_io_spdk_open(const char *traddr, struct nebula_io **out);

#endif /* NEBULA_IO_SPDK_H */
