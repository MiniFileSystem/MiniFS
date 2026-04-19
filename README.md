# Nebula FS

User-space storage system built on the Nebula Device Format.

## Status

| Milestone | Scope | State |
|-----------|-------|-------|
| **M1** | On-disk format + `mkfs` | Done |
| **M2** | Mount lifecycle + operational tools (`fsck`, `label`, `discover`, `mount`) | Done |
| M3 | Block allocator + stream logging + crash recovery | Planned |
| M4+ | File I/O, directory ops, chunk map, replication, ETCD integration | Planned |

## Tools

All tools are built into the `build/` directory.

| Binary | Purpose |
|--------|---------|
| `nebula_format`   | `mkfs` — write a fresh Nebula layout onto a blank image |
| `nebula_dump`     | Raw metadata inspector (hex-dump friendly) |
| `nebula_label`    | Read / write the device MBR standalone (Task A) |
| `nebula_fsck`     | Six-stage checksum and cross-reference validator |
| `nebula_discover` | Scan paths / directories; emit text or JSON with ETCD-ready keys (Task B) |
| `nebula_mount`    | Open a device, pick the latest uberblock, build the hierarchical bitmap, print the resolved mount state |
| `tools/nebula_etcd_push.sh` | Pipes `nebula_discover` JSON into `etcdctl put /nebula/devices/<domain>/<uuid>` (supports `--dry-run`; requires `jq`) |

## Build

Requires a Linux toolchain (`gcc`, `cmake >= 3.16`, `make`). Tested on Ubuntu 24.04.

```bash
cmake -B build
cmake --build build -j
```

### Building from WSL on a Windows-mounted drive

If the source tree lives on a Windows drive (e.g. `/mnt/e/...`), CMake cannot
`chmod` inside `/mnt` and configure will fail with *"Operation not permitted"*.
Put the **build directory** in Linux-native storage and point CMake at the
Windows source path:

```bash
cmake -B ~/nebula-build -S /mnt/e/Desktop/MiniFS
cmake --build ~/nebula-build -j
```

All subsequent commands in this README use `~/nebula-build/...` for that reason.

## Quick Test

```bash
# Create a blank 1 GiB image (outside the source tree, on a data drive)
truncate -s 1G /mnt/d/nebula/disk.img

# Format it
~/nebula-build/nebula_format --path /mnt/d/nebula/disk.img

# Validate every on-disk structure
~/nebula-build/nebula_fsck --path /mnt/d/nebula/disk.img
# expected: "Summary: 0 error(s), 0 warning(s)  CLEAN"

# Mount it (in-memory; no FUSE yet)
~/nebula-build/nebula_mount --path /mnt/d/nebula/disk.img

# Inspect raw fields and first two blocks
~/nebula-build/nebula_dump --path /mnt/d/nebula/disk.img
xxd /mnt/d/nebula/disk.img | head -2

# Label round-trip
~/nebula-build/nebula_label read  --path /mnt/d/nebula/disk.img
~/nebula-build/nebula_label write --path /mnt/d/nebula/disk.img --uuid auto

# Discover devices under a directory, emit JSON for ETCD
~/nebula-build/nebula_discover --domain rack-42 --format json /mnt/d/nebula/
```

## On-Disk Layout

```
LBA 0                  MBR                 (4 KB)
LBA 1                  Superblock (head)   (4 KB)
LBA 2..129             Uberblock region    (128 slots × 4 KB)
LBA 130                Allocator roots H   (4 KB: 64 × 64 B)
LBA 131..N             Bitmap pages
LBA N+1..M             Stream map region   (256 blocks = 1 MiB)
LBA M+1..P             Inode page          (scaled; 128 MiB max)
LBA P+1..Q             Directory page      (scaled; 128 MiB max)
LBA Q+1..end-2         Data region
LBA end-1              Allocator roots T   (4 KB)
LBA end                Superblock (tail)   (4 KB)
```

All LBAs are 4 KiB block numbers. Every on-disk structure carries a CRC32C
checksum computed with the checksum field zeroed.

### Minimum device size

256 MiB. Smaller devices are rejected by `nebula_format` because the inode
and directory pages would not fit.

## Repository Layout

```
include/nebula/          Public headers (on-disk structs, I/O and mount APIs)
src/util/                CRC32C, UUID, logging
src/io/                  POSIX block-I/O backend (pread/pwrite)
src/nebula/              Core library: layout, readers/writers for each region,
                         hierarchical bitmap, mount lifecycle
tools/                   One CLI per binary listed in the Tools table
CMakeLists.txt           Build definitions
```

## Milestones in Detail

### Milestone 1 — Format

Every on-disk structure defined and round-trippable:
MBR, dual superblocks (head + tail), 128-slot uberblock region,
128 allocator roots (split 64 head + 64 tail), bitmap pages,
stream map region, inode page with root inode (`/`, inode 0, type DIR),
directory page with the `"/"` entry.

`mkfs` pre-marks every metadata LBA (MBR, SBs, uberblocks, allocator roots,
bitmap, stream map, inode page, directory page) as allocated in the bitmap
so only data-region blocks appear free on mount (~196 209 free blocks on a
1 GiB image).

Inode size is configurable at format time via `--inode-size 4096|8192`
(default 4096) and recorded in the superblock.

### Milestone 2 — Mount + Tools

- **Hierarchical bitmap** (L0 raw bits, L1 group summaries, L2 total free)
  built in memory from on-disk bitmap pages.
- **Mount lifecycle**: read MBR → validate head+tail SB (with fallback) →
  scan all 128 uberblock slots and pick highest valid TXG → load bitmap →
  (stream replay deferred to M3).
- **`nebula_fsck`** performs six independent validations with structured
  pass / warn / fail reporting and exit codes (`0` clean, `1` warn, `2` fail).
- **`nebula_label write`** updates the MBR **and** rewrites head+tail
  superblocks with the new UUID so `nebula_mount` / `nebula_fsck`
  cross-checks stay consistent.
- **`nebula_discover`** walks files or directories, recognises Nebula
  devices by their MBR, and emits text or JSON tagged with failure
  domain and hostname; JSON includes an `etcd_key` field.
- **`nebula_etcd_push.sh`** completes the ETCD publish path end-to-end:
  it consumes the discover JSON with `jq` and calls
  `etcdctl put /nebula/devices/<domain>/<uuid>` for each device
  (use `--dry-run` to just print the commands).

### Design-doc scaffolding landed in M1+M2 (wired in M3+)

- `struct nebula_stream_record` (1 byte, FREE=0 / ALLOC=1) per design §9.
- `struct nebula_root_chunk_bitmap` + `nebula_root_sub_alloc.{c,h}` —
  4 KiB page tracking the 32768 sub-blocks inside each 128 MiB root-inode
  chunk; pure-memory API (init / alloc / free / free_slots) ready for the
  M3+ `mkdir` flow that hands out 32 KiB directory slots.

## Known Limitations

- Stream map replay is a no-op in M2 (the region is empty after `mkfs`).
  The record format is defined, but the replay loop and crash-recovery
  logic arrive with M3.
- The root-inode 32 KiB sub-allocator exists as an API only — no `mkdir`
  yet wires it into the inode extent map; that lands with M3.
- No FUSE mount yet. `nebula_mount` only validates that the device *can*
  be mounted and prints the resolved state.

## License

TBD.
