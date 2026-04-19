# Nebula FS

User-space storage system built on the Nebula Device Format.

## Milestone 1: Minimal `mkfs`

Initializes a blank device image with the full Nebula on-disk format:
MBR, dual superblocks, uberblock slot 0, allocator roots, bitmap,
stream map region, inode page (with root inode), directory page.

## Build (Linux / WSL)

```bash
cmake -B build
cmake --build build -j
```

## Quick Test

```bash
# Create a blank 1 GiB image
truncate -s 1G /mnt/d/nebula/disk.img

# Format it
./build/nebula_format --path /mnt/d/nebula/disk.img

# Inspect
./build/nebula_dump --path /mnt/d/nebula/disk.img
xxd /mnt/d/nebula/disk.img | head -2
```

## Layout (Milestone 1)

```
LBA 0                  MBR                 (4 KB)
LBA 1                  Superblock (head)   (4 KB)
LBA 2..129             Uberblock region    (128 × 4 KB)
LBA 130                Allocator roots H   (4 KB: 64 × 64 B)
LBA 131..N             Bitmap pages
LBA N+1..M             Stream map region
LBA M+1..P             Inode page          (128 MiB default)
LBA P+1..Q             Directory page      (128 MiB default)
LBA Q+1..end-2         Data region
LBA end-1              Allocator roots T   (4 KB)
LBA end                Superblock (tail)   (4 KB)
```

All LBAs are 4 KB block numbers.
