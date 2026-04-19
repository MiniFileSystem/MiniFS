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
 *
 * traddr can be ONE of:
 *   PCIe BDF  "0000:01:00.0"
 *             NULL = first available local NVMe
 *   TCP       "tcp:<ip>:<port>:<nqn>"
 *             e.g. "tcp:172.31.1.2:4420:nqn.2024-01.com.minifs:nvme0"
 *
 * out: receives the nebula_io handle on success.
 * Returns NEBULA_OK or -errno equivalent.
 *
 * Prerequisites:
 *   - nebula_spdk_env_init() must have been called.
 *   - PCIe: device unbound from kernel + bound to vfio-pci or uio_pci_generic.
 *   - TCP:  hugepages set up; no vfio needed; target reachable on given port.
 */
int nebula_io_spdk_open(const char *traddr, struct nebula_io **out);

/*
 * Convenience wrapper: open an NVMe/TCP device explicitly.
 * ip:    target IPv4 address string e.g. "172.31.1.2"
 * port:  target port string e.g. "4420"
 * nqn:   subsystem NQN e.g. "nqn.2024-01.com.minifs:nvme0"
 * out:   receives the nebula_io handle on success.
 * Returns NEBULA_OK or -errno equivalent.
 */
int nebula_io_spdk_open_tcp(const char *ip, const char *port,
                            const char *nqn, struct nebula_io **out);

#endif /* NEBULA_IO_SPDK_H */
