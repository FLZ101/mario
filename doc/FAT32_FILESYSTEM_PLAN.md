# FAT32 Filesystem Support Plan

## Goal

Add a kernel filesystem driver for **FAT32**, using the existing block-device
and VFS layers. The first milestone is reliable read-only support. A later
milestone adds the writable operations needed by the shell test suite:
create, unlink, mkdir, rmdir, file read/write/truncate, and rename.

The target is plain FAT32 as created by `mkfs.fat -F 32`. FAT12 and FAT16 are
out of scope. The bootloader's FAT32 reader in `src/boot/boot.asm` remains
unchanged; the kernel driver is a separate implementation.

## Dependencies

The AHCI hard-disk driver described in `doc/HARDDISK_DRIVER_PLAN.md` is
implemented and exposes MBR/GPT partitions as block devices. With the three
test disks attached by `qemu/Makefile`, the mapping is:

- `qemu/hd1.img` -> `/dev/hda`
- `qemu/hd2.img` -> `/dev/hdb`
- `qemu/hd3.img` -> `/dev/hdc`

The image scripts already format several FAT32 test partitions:

```sh
# qemu/make-hd1.sh
sudo mkfs -t fat -F 32 ${loopdev}p1   # /dev/hda1 boot/GRUB partition
sudo mkfs -t fat -F 32 ${loopdev}p7   # /dev/hda7 kernel/initrd update partition

# qemu/make-hd2.sh
sudo mkfs -t fat -F 32 ${loopdev}p3   # /dev/hdb3 primary FAT32
sudo mkfs -t fat -F 32 ${loopdev}p5   # /dev/hdb5 logical FAT32

# qemu/make-hd3.sh
sudo mkfs -t fat -F 32 ${loopdev}p1   # /dev/hdc1 GPT FAT32
sudo mkfs -t fat -F 32 ${loopdev}p2   # /dev/hdc2 GPT FAT32
```

These are natural read-only and writable test targets. The ramdisk-based
MarioFS root filesystem remains unchanged.

## Current VFS State

The kernel already has the generic mount, inode, namei, read/write, and
getdents machinery, and `src/fs/ext2/` is a working second on-disk filesystem.
FAT32 should follow the ext2 integration path:

- Add `struct fat_sb_info` / `struct fat_inode_info` and add them to the
  private-data unions in `src/include/fs/fs.h`; ext2 members are already there.
- Register `struct file_system_type vfat` in `src/fs/fs.c`, next to `ext2`.
- `statfs64_from_inode()` now has a FAT32 branch in `src/fs/stat.c`; ext2 is
  still rejected until that helper is generalized further.
- `/proc/mounts` in `src/fs/proc.c` is now generated from mount records and
  reports source, mountpoint, filesystem type, and read-only state.

The most important difference from ext2 is that FAT has no native inode table
and no fixed-size logical block. FAT32 data is addressed by 32-byte directory
entries, FAT cluster chains, and clusters whose size varies by filesystem.

## Filesystem Choice

Implement FAT32 in `src/fs/fat/` with public structures in
`src/include/fs/fat/fat.h`.

`src/Makefile` automatically compiles `*/*/*.c`, so no build-list change is
required.

Register the filesystem under the conventional mount name:

```c
struct file_system_type fat_fs_type = {
    .read_super = fat_read_super,
    .name = "vfat",
};
```

An additional alias such as `"fat32"` may be registered later if desired.

## FAT32 On-Disk Layout

### BPB

The FAT32 BIOS Parameter Block starts at sector 0 of the mounted block device.
The kernel only needs a subset:

| BPB offset | Field |
|---|---|
| 0x0b | bytes per sector |
| 0x0d | sectors per cluster |
| 0x0e | reserved sector count |
| 0x10 | number of FATs |
| 0x13 | 16-bit total sector count, zero for FAT32 |
| 0x20 | 32-bit total sector count |
| 0x24 | FAT size in sectors, FAT32 |
| 0x2c | root directory cluster |
| 0x30 | FSInfo sector |

Validate the filesystem:

- `bytes_per_sector == 512`; the current block layer is 512-byte oriented.
- `sectors_per_cluster` is a power of two and within a reasonable limit
  (for example 1..128).
- `reserved_sector_count > 0`.
- `num_fats` is 1 or 2; writable support updates every FAT copy.
- `fat_size32 > 0`.
- `root_cluster >= 2`.
- Total sectors comes from the 32-bit field when the 16-bit field is zero.

Compute:

```c
cluster_size = bytes_per_sector * sectors_per_cluster;
fat_start = reserved_sector_count;
first_data_sector = reserved_sector_count + num_fats * fat_size32;
data_sectors = total_sectors - first_data_sector;
total_clusters = data_sectors / sectors_per_cluster;
first_cluster_sector(cluster) =
    first_data_sector + (cluster - 2) * sectors_per_cluster;
```

All sector numbers are relative to the mounted partition. The hard-disk driver
is responsible for translating partition minors to absolute disk LBAs.

### FAT Table

FAT32 uses 32-bit entries, but only the low 28 bits are valid:

```c
fat_value = le32_entry & 0x0fffffff;
```

Interpretation:

- `0x00000000`: free cluster.
- `0x00000001`: reserved.
- `0x0ffffff7`: bad cluster.
- `0x0ffffff8..0x0fffffff`: end of chain.
- otherwise: next cluster in the chain.

The FAT sector containing cluster `c` is:

```c
fat_sector = fat_start + (c * 4) / bytes_per_sector;
fat_offset = (c * 4) % bytes_per_sector;
```

Add a small FAT-sector cache in `fat_sb_info` because directory traversal and
file reads repeatedly access nearby FAT entries. A single cached FAT sector is
enough for the first version; a small LRU can be added later.

### Data Clusters

Data starts immediately after the FATs:

```c
data_sector = first_data_sector +
              (cluster - 2) * sectors_per_cluster;
```

Use a FAT-specific cluster reader:

```c
int fat_read_cluster(struct super_block *sb, uint32_t cluster, void *buf);
int fat_write_cluster(struct super_block *sb, uint32_t cluster,
                      const void *buf);
```

It calls `blkdev_read()`/`blkdev_write()` for `sectors_per_cluster` sectors.
`kmalloc(cluster_size)` is sufficient for normal FAT32 cluster sizes; use the
page allocator if very large clusters are later supported.

## Directory Entries

Directory entries are 32 bytes:

| Offset | Field |
|---|---|
| 0 | short name, 11 bytes, or first byte for deleted/end markers |
| 11 | attributes |
| 13 | reserved |
| 14 | creation time |
| 16 | creation date |
| 18 | last access date |
| 20 | high cluster word |
| 22 | write time |
| 24 | write date |
| 26 | low cluster word |
| 28 | file size in bytes |

Important attribute bits:

- `0x01` read-only
- `0x02` hidden
- `0x04` system
- `0x08` volume label
- `0x10` directory
- `0x20` archive

Deleted entries have first byte `0xe5`; the end marker is `0x00`.

### Long File Names

LFN entries have attribute `0x0f` and contain:

- sequence number plus last-entry flag,
- UTF-16LE name characters,
- a checksum of the associated short name.

Read support should:

- Accumulate LFN entries preceding a short-name entry.
- Validate the checksum.
- Convert UTF-16LE to the kernel's byte-oriented name space.
- For code points above the supported byte range, use a lossy fallback such as
  `_` or `?`.

Write support may initially generate only valid 8.3 short names; generating
LFN entries can be deferred because the kernel's VFS already has an 8.3
generator in the bootloader but not a reusable one.

## Inode Model

FAT has no on-disk inode numbers. The kernel VFS, however, requires a stable
`i_ino` per object.

Use the **physical directory-entry location** as the inode number, similar to
how MarioFS already uses directory-entry offset:

```c
#define FAT_ROOT_INO 1

ino = ((cluster - 2) * cluster_size + entry_offset) + FAT_ROOT_INO + 1;
```

Here `cluster` is the actual data cluster containing the 32-byte short-name
entry, and `entry_offset` is its byte offset within that cluster. This value
is unique for every live directory entry and can be decoded back to a cluster
and offset by `fat_read_inode()`.

The root directory is represented by `FAT_ROOT_INO`; child entries use the
physical-entry formula.

`struct fat_inode_info` stores at least:

- starting cluster
- cluster containing the short-name entry
- byte offset of the short-name entry within that cluster
- file attributes
- cached FAT-chain information for regular files

Because the inode number points at the short-name entry, `fat_read_inode()` can
read that entry without knowing the parent directory chain. LFN reconstruction
inside `fat_read_inode()` is not required for metadata; lookup and readdir
already know the long name and can store it in `fat_inode_info` after `iget()`.

## VFS Integration

### `src/include/fs/fs.h`

Include:

```c
#include <fs/fat/fat.h>
```

Extend the private-data unions:

```c
union {
    struct mario_sb_info mario_sb;
    struct fat_sb_info fat_sb;
} u;

union {
    struct mario_inode_info mario_i;
    struct pipe_inode_info pipe_i;
    struct fat_inode_info fat_i;
} u;
```

Use the same `super_operations.statfs` extension proposed for ext2 so FAT32
can report its cluster-based geometry.

### `src/fs/fs.c`

Register `fat_fs_type`. `fat_read_super()` must fail cleanly for the MarioFS
ramdisk root so registering it before `mount_root()` is safe.

### `src/fs/stat.c`

FAT32 `statfs` fills:

- `f_bsize = cluster_size`
- `f_frsize = cluster_size`
- `f_blocks = total_clusters`
- `f_bfree = free_clusters`
- `f_files = 0`
- `f_ffree = 0`
- `f_namelen = 255` once LFN support is present

For read-only support, free clusters can be counted during mount or read from
FSInfo if it is valid. For writable support, keep the count in
`fat_sb_info`.

### `src/fs/proc.c`

Show `vfat` mounts in `/proc/mounts` instead of always printing MarioFS.

## Superblock and Mounting

`fat_read_super()`:

1. Read sector 0 and validate the BPB.
2. Compute geometry and reject FAT12/FAT16.
3. Read the FAT32-specific BPB fields.
4. Validate cluster count and supported attributes.
5. Fill `fat_sb_info`.
6. Set `sb->s_block_size = cluster_size`.
7. Set `sb->s_magic = FAT32_SUPER_MAGIC`.
8. Set `sb->s_op = &fat_super_ops`.
9. Create the root inode with `FAT_ROOT_INO`.

Read-only mounts set `fat_sb_info.readonly = 1` until write support is
complete.

## Inode Operations

### `fat_read_inode`

- If `i_ino == FAT_ROOT_INO`, populate the root directory from
  `sb->u.fat_sb.root_cluster`.
- Otherwise decode the physical directory-entry offset into `cluster` and
  `entry_offset`, read that cluster, and load the short-name entry.
- Map attributes to `i_mode`:
  - directory: `S_IFDIR | 0777`
  - volume label or system/hidden: skip or expose as regular
  - otherwise: `S_IFREG | 0777`
- Set `i_size`, `i_nlink`, timestamps, `i_block_size`, and `i_nr_block`.
- Assign operation tables:
  - directory: `fat_dir_iops`, `fat_dir_fops`
  - regular file: `fat_file_iops`, `fat_file_fops`
- Set `i_op->truncate` for regular files in the writable milestone.

### `fat_write_inode`

Decode `i_ino` back to the short-name directory entry and write size, starting
cluster, attributes, and timestamps. This is required only for writable FAT.

### `fat_clear_inode`

Release any cached FAT-chain mapping. Do not free the embedded `fat_inode_info`
itself.

## FAT Chain and File Data

### Read Path

`fat_file_read()`:

1. Clamp the request to `i_size`.
2. Walk the FAT chain from the starting cluster to the file-offset cluster.
3. Read the containing cluster.
4. Copy the requested byte range.
5. Advance `f->f_pos`.

Walking the FAT chain from the start on every read is correct but slow for
random seeks. Cache either the last resolved file cluster or a full
cluster-chain map in `fat_inode_info`, similar to the MarioFS block-map
optimization.

### Write Path

`fat_file_write()`:

- Lock `inode->i_sem`.
- Handle `O_APPEND`.
- Allocate clusters when extending the file.
- Read-modify-write partial clusters.
- Update the directory entry and mark the inode dirty.

### Truncate

`fat_truncate()`:

- Extend by allocating clusters and linking the FAT chain.
- Shrink by freeing the tail of the cluster chain.
- Update file size and the directory entry.

## FAT Allocation

Add `src/fs/fat/alloc.c` for writable support:

- `fat_next_cluster(sb, cluster)` reads the FAT table.
- `fat_set_cluster(sb, cluster, value)` updates all FAT copies and the cache.
- `fat_alloc_cluster(sb, near)` scans for a free FAT entry and marks it end of
  chain.
- `fat_free_cluster_chain(sb, cluster)` walks and frees a chain.

Serializing allocation with `sb->s_sem` prevents two tasks from choosing the
same free cluster. The first allocator can be a linear scan; FSInfo's
`next_free` hint can be added later.

## Directory Operations

### Lookup

`fat_lookup()`:

- Handle `.` by returning the directory inode.
- Walk the directory cluster chain.
- Accumulate LFN entries and validate their checksum.
- Compare both LFN and short names case-insensitively.
- Return `iget(sb, physical_entry_ino)`.

### Readdir

`fat_readdir()`:

- Treat `f_pos` as a directory byte offset.
- Skip deleted and volume-label entries.
- Emit each LFN/short-name group once with `IFTODT(mode)`.
- Advance `f_pos` past the emitted short-name entry.
- Stop at the `0x00` end marker.

The first version can emit short names only. LFN emission is the next
read-only sub-milestone.

### Create/Mkdir/Unlink/Rmdir

Read-only FAT leaves these as `NULL` or returns `-EROFS`.

Writable FAT:

- `fat_create()` allocates a cluster if needed and inserts a short-name entry.
- `fat_mkdir()` allocates a directory cluster, writes `.` and `..`, and links
  the directory into its parent.
- `fat_unlink()` removes the directory entry and frees the FAT chain.
- `fat_rmdir()` removes the entry after checking that the directory is empty.

### Rename

Implement rename after the simpler operations. Initial support can handle
same-directory renaming of short names; moving a directory entry and
reassigning synthetic inode numbers requires invalidating the old VFS inode.

### Unsupported FAT Features

Set these inode operations to `NULL` so the VFS returns an error:

- `mknod`: FAT has no device nodes.
- `symlink`, `readlink`, `follow_link`: FAT has no native symlinks.
- `link`: FAT has no hard links.

## Files to Change

### New

- `src/include/fs/fat/fat.h`
- `src/fs/fat/super.c`
- `src/fs/fat/inode.c`
- `src/fs/fat/dir.c`
- `src/fs/fat/file.c`
- `src/fs/fat/fat.c`
- `src/fs/fat/alloc.c`
- `src/fs/fat/misc.c` (timestamp and name conversions)

### Modified

- `src/include/fs/fs.h`
- `src/fs/fs.c`
- `src/fs/stat.c`
- `src/fs/proc.c`

No change to `src/Makefile` is required.

## Milestones

### Milestone 1: Read-only mount and short names

- Parse the FAT32 BPB and validate geometry.
- Register `"vfat"`.
- Implement FAT-sector cache and cluster reads.
- Implement root inode, short-name lookup, and short-name readdir.
- Implement regular-file read through FAT chains.
- Mount a FAT32 partition and pass `ls`/`cat` tests using short names.

### Milestone 2: Long file names and statfs

- Parse and validate LFN entries in lookup and readdir.
- Convert UTF-16LE names lossily to kernel byte names.
- Integrate `statfs` and `/proc/mounts`.
- Verify with files created by host `mkfs.fat`/`cp`.

### Milestone 3: Writable files

- Implement `fat_write_inode` and FAT chain allocation/free.
- Implement file write, extend, and truncate.
- Implement create and unlink.
- Verify with `touch`, `echo >`, `cp`, `rm`, and `truncate`.

### Milestone 4: Writable directories and rename

- Implement mkdir, rmdir, and same-directory rename.
- Add directory entry allocation and free-slot scanning.
- Add LFN generation or keep write-side names to valid 8.3 form.
- Verify with `mkdir`, `rmdir`, `mv`, and larger shell tests.

### Milestone 5: Performance and hardening

- Add a cluster-chain cache for regular files.
- Add a small FAT-sector LRU and cluster buffer cache.
- Add corruption checks for bad FAT values and directory loops.
- Update mount/umount dirty-state handling.

## Validation

After the hard-disk driver is built, mount the existing boot FAT partition
from `qemu/hd1.img`:

```text
mkdir /mnt
mount -t vfat /dev/hda1 /mnt
ls -la /mnt
cat /mnt/menu.lst
```

`/dev/hda1` already contains short and long names (`grldr`, `menu.lst`).
`/dev/hda7` is the update-image FAT partition and contains `kernel.exe`,
`boot.bin`, and `rdx`.

`qemu/hd2.img` provides two dedicated FAT32 data partitions, `/dev/hdb3` and
`/dev/hdb5`, and `qemu/hd3.img` provides two GPT FAT32 partitions, `/dev/hdc1`
and `/dev/hdc2`. These are populated with small test files and are convenient
for read-only tests.

For writable tests, use `qemu-system-i386 -snapshot` or a separate disposable
FAT32 image. Do not write to the checked-in `qemu/hd1.img`, `qemu/hd2.img`, or
`qemu/hd3.img` in normal tests.

Host-side image creation:

```sh
truncate -s 64M fat32.img
mkfs.fat -F 32 fat32.img
mmd -i fat32.img ::/dir
echo hello | mcopy -i fat32.img - ::/dir/hello.txt
```

Then expose `fat32.img` as an AHCI disk in QEMU and mount it.

Automated test candidates:

- `tests/shell/vfat.test`: implemented with `REQUIRES-HD:` and mounts, lists,
  and reads FAT32 partitions across the generated test disks.
- `tests/shell/vfat-write.test`: use `REQUIRES-HD:` and create/write/read/
  delete/rename with `-snapshot` so the generated images stay clean.
- A small `app/test/fat` program for deterministic BPB/FAT/directory checks.

## Deferred

- FAT12 and FAT16.
- LFN generation for writes.
- Unicode normalization and full UTF-8 output for long names.
- FSInfo `next_free` allocation hints.
- Dirty-volume bit, free-cluster count maintenance, and FAT mirror handling.
- Defragmentation or contiguous cluster allocation.
- `mmap` for FAT files.

## Risks

- FAT32 BPB fields are unaligned; use fixed-width types and avoid assuming
  natural C struct layout.
- Inode numbers synthesized from directory-entry offsets change when a file is
  renamed or its directory entry is relocated; rename support must invalidate
  the old inode.
- FAT chains are singly linked, so backward seeks naively require a walk from
  the file start. A chain cache is important for acceptable performance.
- LFN entries can span directory entries and have checksum/sequence validation;
  incorrect parsing can merge or lose names.
- Writes must update every FAT copy and keep directory entry, FAT, and file
  data consistent.
- `mkdir`/`rmdir` must correctly create and validate `.` and `..` entries to
  avoid corrupt directory cycles.
