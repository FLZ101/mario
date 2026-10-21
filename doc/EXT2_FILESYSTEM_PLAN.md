# Ext2 Filesystem Support Plan

## Goal

Add a kernel filesystem driver for plain Linux **ext2** disks, using the
existing block-device and VFS layers. The first milestone is a reliable
read-only implementation. A later milestone adds write support for the core
operations needed by the shell test suite: create, unlink, mkdir, rmdir, file
write/truncate, symlink, and mknod.

The target is ordinary ext2, not ext3/ext4. In particular, journaling, extents,
and 64-bit ext4 features are out of scope.

## Dependencies

This plan assumes the hard-disk block driver from
`doc/HARDDISK_DRIVER_PLAN.md` is available and registers block devices such as
`/dev/hda5`. GPT partitions from `doc/GPT_SUPPORT_PLAN.md` are not required by
ext2 itself, but the generated `qemu/hd3.img` uses GPT and is now a convenient
dedicated ext2 test disk.

The existing `qemu/gen_image.sh` already formats several ext2 test partitions:

```sh
sudo mkfs -t ext2 ${loopdev}p5   # /dev/hda5, MBR logical partition
sudo mkfs -t ext2 ${loopdev2}p1  # /dev/hdb1, MBR primary partition
sudo mkfs -t ext2 ${loopdev2}p6  # /dev/hdb6, MBR logical partition
sudo mkfs -t ext2 ${loopdev3}p3  # /dev/hdc3, GPT partition on hd3.img
sudo mkfs -t ext2 ${loopdev3}p4  # /dev/hdc4, GPT partition on hd3.img
```

The two GPT partitions on `hd3.img` are especially convenient read-only test
targets because they are dedicated to ext2 and are not used by boot or Grub4DOS.
The ramdisk-based MarioFS root filesystem remains unchanged.

## Current VFS State

The kernel already has most of the generic infrastructure needed by ext2:

- `src/fs/super_block.c` manages mounted `struct super_block` objects and calls
  `file_system_type.read_super()`.
- `src/fs/inode.c` provides a global inode cache and calls filesystem-specific
  `read_inode`, `write_inode`, and `clear_inode` operations.
- `src/fs/namei.c` dispatches lookup/create/mkdir/unlink/rename/mknod to
  `struct inode_operations`.
- `src/fs/read_write.c` dispatches file reads/writes to
  `struct file_operations`.
- `src/fs/readdir.c` already implements the generic `getdents` glue.

However, some pieces are currently MarioFS-specific:

- `src/include/fs/fs.h` only has MarioFS in the `super_block` and `inode`
  private-data unions.
- `src/fs/stat.c` refuses `statfs` for anything except `MARIO_MAGIC`.
- `/proc/mounts` in `src/fs/proc.c` is hard-coded to MarioFS.
- The generic buffer cache in `src/fs/block/buffer.c` assumes 512-byte logical
  blocks, while ext2 block sizes are commonly 1024, 2048, or 4096 bytes.

## Filesystem Choice

Implement ext2 in a new `src/fs/ext2/` directory and expose the public header
as `src/include/fs/ext2/ext2.h`.

`src/Makefile` automatically compiles `*/*/*.c`, so no build-list change is
required for the new C files.

## On-Disk Structures

Define little-endian ext2 structures in
`src/include/fs/ext2/ext2.h`. Mario currently runs only on i386, but the header
should still use fixed-width `__u8`/`__u16`/`__u32` types and packed
structures.

### Superblock

The ext2 superblock starts at byte offset 1024 of the device, not at sector 0.
Important fields:

| Offset | Field |
|---|---|
| 0 | inodes count |
| 4 | blocks count |
| 8 | reserved blocks count |
| 12 | free blocks count |
| 16 | free inodes count |
| 20 | first data block |
| 24 | log2 block size |
| 32 | blocks per group |
| 36 | fragments per group |
| 40 | inodes per group |
| 56 | magic, must be `0xef53` |
| 76 | revision level |
| 84 | first non-reserved inode |
| 88 | inode size |
| 92 | compat feature flags |
| 96 | incompat feature flags |
| 100 | read-only compat feature flags |

Compute:

```c
block_size = 1024 << s_log_block_size;
first_data_block = (block_size == 1024) ? 1 : 0;
nr_groups = (blocks_count - first_data_block +
             blocks_per_group - 1) / blocks_per_group;
```

The initial probe can read device sectors 2 and 3 (byte offset 1024), verify
`s_magic`, and obtain `s_log_block_size` before allocating larger buffers.

### Feature Handling

For read-only support:

- Reject unsupported incompat features, especially:
  - journal
  - extents
  - 64-bit
  - meta block groups
  - inline data
- Support `EXT2_FEATURE_INCOMPAT_FILETYPE`, which is set by normal
  `mkfs.ext2` and means directory entries carry a `file_type` byte.
- Compat and most read-only-compat features can be ignored for read access,
  but indexed (`DIR_INDEX`) directories require either a linear-directory
  fallback or explicit rejection.

For writable support:

- Reject metadata checksum and uninitialized-bitmap features unless checksum
  support is implemented.
- Initial writable tests should use:

  ```sh
  mkfs.ext2 -O ^metadata_csum,^64bit,^extents image
  ```

- Do not write to a filesystem with unsupported read-only-compat features.

### Group Descriptors

Each block group has a 32-byte descriptor:

```c
struct ext2_group_desc {
    __u32 bg_block_bitmap;
    __u32 bg_inode_bitmap;
    __u32 bg_inode_table;
    __u16 bg_free_blocks_count;
    __u16 bg_free_inodes_count;
    __u16 bg_used_dirs_count;
    __u16 bg_pad;
    __u32 bg_reserved[3];
};
```

The descriptor table starts in the block immediately after the superblock:

- 1 KiB blocks: logical block 2.
- 2/4 KiB blocks: logical block 1.

Load all descriptors once at mount time into `ext2_sb_info` for both read-only
and writable operation.

### Inodes

The important fixed part of the ext2 inode is:

| Offset | Field |
|---|---|
| 0 | mode |
| 2 | uid |
| 4 | size |
| 8 | atime |
| 12 | ctime |
| 16 | mtime |
| 24 | gid |
| 26 | links count |
| 28 | blocks count, in 512-byte sectors |
| 40 | `i_block[15]` data block pointers |
| 100 | generation |
| 104 | file ACL block |
| 108 | directory ACL / high 32 bits of size for large files |

For the first implementation:

- Use `i_mode` directly with the existing `S_IF*` values.
- Keep a copy of the raw on-disk inode in `ext2_inode_info` so write support
  can preserve fields the kernel does not otherwise expose.
- Use `i_blocks` as `inode->i_nr_block` and `sb->s_block_size` as
  `inode->i_block_size`.
- Set `inode->i_nlink` from `i_links_count`.
- Store the raw `i_block[15]` array in `ext2_inode_info`.

Device nodes encode the device number in the first inode block pointer for the
traditional format. Support decoding and encoding that convention for ext2
`mknod`.

## Block I/O

The existing `bread()` path is 512-byte oriented. Do not change the MarioFS
buffer cache in the first version.

Add an ext2-specific logical-block helper:

```c
int ext2_read_block(struct super_block *sb, uint32_t block, void *buf);
int ext2_write_block(struct super_block *sb, uint32_t block, const void *buf);
```

For block size `N`, this calls:

```c
blkdev_read(sb->s_dev, block * (N / 512), N / 512, buf);
```

The caller allocates a buffer of at least `N` bytes. One `page_alloc()` is
sufficient for the supported block sizes. The first milestone may use
allocate/read/free for each logical block. A later optimization can add a
small per-superblock ext2 block cache; this avoids changing the shared MarioFS
buffer cache.

For writable blocks, `ext2_write_block()` must mark `SB_Dirty` appropriately
and the filesystem must flush pending metadata on `write_super()`.

## VFS Integration

### `src/include/fs/fs.h`

Include the new header:

```c
#include <fs/ext2/ext2.h>
```

Extend private data:

```c
struct super_block {
    ...
    union {
        struct mario_sb_info mario_sb;
        struct ext2_sb_info ext2_sb;
    } u;
};

struct inode {
    ...
    union {
        struct mario_inode_info mario_i;
        struct pipe_inode_info pipe_i;
        struct ext2_inode_info ext2_i;
    } u;
};
```

Add a `statfs` method to `struct super_operations` so filesystems can report
their own geometry:

```c
int (*statfs)(struct super_block *, struct statfs64 *);
```

### `src/fs/fs.c`

Register the new filesystem:

```c
register_file_system(&ext2);
```

`ext2_read_super()` must fail cleanly for the MarioFS ramdisk root, so
registering it before `mount_root()` is safe.

### `src/fs/stat.c`

Change `statfs64_from_inode()` to use `sb->s_op->statfs` when present. Keep the
existing MarioFS logic as the fallback or as the MarioFS callback
implementation.

Ext2 fills:

- `f_bsize = f_frsize = block_size`
- `f_blocks = blocks_count`
- `f_bfree = free_blocks_count`
- `f_files = inodes_count`
- `f_ffree = free_inodes_count`
- `f_namelen = EXT2_NAME_LEN - 1`

### `src/fs/proc.c`

Update `/proc/mounts` so a mounted ext2 filesystem is shown instead of always
printing MarioFS. This can be a small mount-table walk or a second hard-coded
entry during the initial implementation.

## Inode Operations

### Read Inode

`ext2_read_inode()`:

1. Compute the group containing `i_ino`.
2. Read the group descriptor.
3. Compute the inode-table block:
   `block = inode_table + (inode_index * inode_size) / block_size`.
4. Read the inode-table block and copy the raw inode into
   `i->u.ext2_i.raw`.
5. Populate the generic `struct inode` fields.
6. Assign operation tables:
   - regular file: `ext2_file_iops`, `ext2_file_fops`
   - directory: `ext2_dir_iops`, `ext2_dir_fops`
   - symlink: `ext2_symlink_iops`
   - char/block device: existing `chrdev_fops`/`blkdev_fops`
   - FIFO: existing `fifo_fops`

Do not mark the generic inode dirty during read.

### Write Inode

`ext2_write_inode()` copies the generic size, mode, link count, timestamps, and
block count back into the cached raw inode, then writes the inode-table block.
This is only required for writable ext2.

### Clear Inode

`ext2_clear_inode()` frees any allocated ext2 inode cache, such as a cached
logical-to-physical block map, but must not free the raw inode copy itself if
it is embedded in the union.

## Block Mapping

Implement the standard ext2 block mapping:

- Direct blocks: `i_block[0..11]`.
- Single indirect: `i_block[12]`.
- Double indirect: `i_block[13]`.
- Triple indirect: `i_block[14]`.

Read path:

```c
uint32_t ext2_bmap(struct inode *inode, uint32_t file_block, int create);
```

- Return 0 for a hole; callers must zero-fill reads.
- For directories and normal files, cache at least the most recently resolved
  block to keep sequential reads efficient.
- For writable operation, `create != 0` allocates indirect blocks and data
  blocks through the block allocator.

The first version can implement only the read path. The create path belongs to
the write milestone.

## Directory Operations

Ext2 directories are arrays of variable-length entries:

```c
struct ext2_dir_entry_2 {
    __u32 inode;
    __u16 rec_len;
    __u8  name_len;
    __u8  file_type;
    char  name[];
};
```

### Lookup

`ext2_lookup()`:

- Handle `.` by returning the directory inode.
- Use `ext2_bmap()` to iterate directory logical blocks.
- Walk entries using `rec_len`.
- Skip deleted entries where `inode == 0`.
- Return `iget(sb, entry.inode)`.

If the directory has the ext2 index flag and htree indexing is not
implemented, return `-ENOTSUP` instead of doing an incorrect linear search.

### Readdir

`ext2_readdir()` treats `f_pos` as a byte offset into the directory. It should:

- Advance past deleted entries but not emit them.
- Emit each valid entry with its inode number and use `file_type` directly;
  ext2 directory `file_type` values already use the same numbering as dirent
  `DT_*` values.
- Preserve `f_pos` semantics so `getdents`, `telldir`, and `seekdir` work.
- Handle directory blocks larger than the user-supplied getdents buffer.

### Create/Mkdir/Unlink/Rmdir/Mknod/Symlink

Read-only ext2 sets these pointers to `NULL` or returns `-EROFS`.

Writable ext2 adds:

- `ext2_create()`: allocate an inode, add a directory entry, return the inode.
- `ext2_mkdir()`: allocate an inode, create `.` and `..`, link into parent.
- `ext2_unlink()`/`ext2_rmdir()`: remove a directory entry and free the inode
  when its link count reaches zero.
- `ext2_mknod()`: allocate an inode and encode the device number.
- `ext2_symlink()`: use fast symlinks when the target fits in `i_block[]`;
  otherwise allocate a data block.

### Rename

`rename` is more complex because of directory cycles and cross-directory moves.
Implement it after the simpler directory operations. Until then, leave
`rename` as `NULL` so `rename()` returns `-EPERM`, or implement only same-parent
renames first.

## Regular File Operations

### Read

`ext2_file_read()`:

- Clamp to `inode->i_size`.
- Resolve the file block with `ext2_bmap()`.
- Read the block with `ext2_read_block()`.
- Copy the requested bytes with `memcpy_tofs()`.
- Zero-fill holes.
- Advance `f->f_pos`.

### Write

`ext2_file_write()`:

- Lock `inode->i_sem`, as MarioFS does.
- Handle `O_APPEND`.
- Map the target blocks with allocation enabled.
- Read-modify-write partial blocks.
- Allocate new direct/indirect blocks as needed.
- Update `i_size`, `i_blocks`, `i_mtime`, and mark the inode dirty.

### Truncate

`ext2_truncate()`:

- Free data blocks above the new size.
- Free single/double/triple indirect blocks that become empty.
- Update `i_size`, `i_blocks`, and dirty state.

## Block and Inode Allocation

Writable ext2 needs two allocators:

- `src/fs/ext2/balloc.c`
  - Read a group's block bitmap.
  - Find and mark a free bit.
  - Update the group descriptor and superblock free-block count.
- `src/fs/ext2/ialloc.c`
  - Read a group's inode bitmap.
  - Allocate and zero an inode.
  - Update the group descriptor and superblock free-inode count.

Use `sb->s_sem` or a dedicated ext2 metadata lock around bitmap and count
updates. Deallocation is symmetric and is required by `unlink`, `rmdir`, and
`truncate`.

The first allocator can be simple linear scanning. Per-directory block-group
heuristics and block preallocation are deferred.

## Read-Only vs Writable Enforcement

The first ext2 mount should be internally read-only:

- `ext2_read_super()` sets `ext2_sb_info.readonly = 1`.
- File write/truncate and directory mutation operations return `-EROFS`.
- `write_super` and `write_inode` can remain unimplemented.

After the writable milestone, remove that internal flag and enforce
`MS_RDONLY` only if the VFS is extended to pass mount flags into
`read_super()` or the superblock stores a read-only flag.

## Files to Change

### New

- `src/include/fs/ext2/ext2.h`
- `src/fs/ext2/super_block.c`
- `src/fs/ext2/inode.c`
- `src/fs/ext2/bmap.c`
- `src/fs/ext2/dir.c`
- `src/fs/ext2/file.c`
- `src/fs/ext2/balloc.c`
- `src/fs/ext2/ialloc.c`
- `src/fs/ext2/buffer.c` (logical block I/O/cache)

### Modified

- `src/include/fs/fs.h`
- `src/fs/fs.c`
- `src/fs/stat.c`
- `src/fs/proc.c`
- `src/fs/block/blkdev.c`
- `src/fs/super_block.c`
- `src/kernel/mario.c`

No changes to `src/Makefile` are needed.

## Direct Ext2 Root (Option A)

This mode makes an ext2 partition the true `/` from the start of userspace.
The ramdisk is still loaded as an initrd module, but it is not mounted as the
root filesystem. This is the simpler option when the ext2 image is known at
boot time and does not need an initrd pivot into it.

### Root device selection

Replace the hard-coded ramdisk root in `src/fs/block/blkdev.c`:

```c
ROOT_DEV = MKDEV(RD_MAJOR, 0);
```

with a boot-parameter parser. The Multiboot command line is available from
`m->cmdline` after converting it from the physical address to the kernel
virtual address:

```c
void setup_root_device(struct multiboot_info *m)
{
    const char *cmdline = (const char *)(m->cmdline + KERNEL_BASE);

    ROOT_DEV = parse_root_device(cmdline);
    if (!ROOT_DEV)
        ROOT_DEV = MKDEV(RD_MAJOR, 0);
}
```

Call this before `blkdev_init()`, or change `blkdev_init()` to accept `m`.
`blkdev_init()` must no longer overwrite `ROOT_DEV` unconditionally.

`parse_root_device()` should accept a Linux-style device name:

- `root=/dev/rd0` keeps the current ramdisk behavior.
- `root=/dev/hda5` uses the MBR ext2 partition on `hd.img`.
- `root=/dev/hdc3` uses the first GPT ext2 partition on `hd3.img`.

With the current AHCI minor layout, a disk minor is:

```c
disk_minor = disk_index * HD_MINORS_PER_DISK;
partition_minor = disk_minor + partition_number;
```

For `/dev/hdc3`, this is minor `2 * 17 + 3 = 37`, giving
`MKDEV(HD_MAJOR, 37)`.

### Root mount

Register ext2 in `src/fs/fs.c` before `mount_root()`:

```c
register_file_system(&mariofs);
register_file_system(&ext2);
mount_root();
```

Make `mount_root()` deterministic by stopping at the first filesystem whose
`read_super()` succeeds, or by passing the requested filesystem name. With an
ext2 root device, `mario_read_super()` fails and `ext2_read_super()` succeeds;
with the default ramdisk root, the opposite happens.

`ext2_read_super()` must return `NULL` cleanly for non-ext2 devices so the
ramdisk default remains unchanged.

### Userspace init path

Change `src/kernel/mario.c` from the fixed ramdisk executable:

```c
execve("/bin/example/init.exe", argv_init, envp_init);
```

to either `/sbin/init` or a value parsed from `init=/path` on the command line.
The ext2 root image must then contain that init program and its runtime files.

### Device nodes on the ext2 root

Direct ext2 root does not inherit the ramdisk `/dev` tree. Populate the ext2
image on the host with static device nodes for the console and serial devices
needed by init, plus the block devices used by the root filesystem:

```text
/dev/console
/dev/ttyS0
/dev/hdc3
```

For an initially read-only ext2 driver, create these nodes before packaging
the image on the host.

### Boot test

With `hd3.img` as the only AHCI disk, it is `/dev/hda`, so its third GPT
partition is `/dev/hda3`:

```sh
qemu-system-i386 \
  -kernel src/kernel/kernel.exe -initrd rd/rdx \
  -drive id=root,file=qemu/hd3.img,format=raw,if=none \
  -device ahci,id=ahci \
  -device ide-hd,drive=root,bus=ahci.0 \
  -append "root=/dev/hda3 init=/sbin/init console=/dev/ttyS0" \
  -display none -serial stdio -no-reboot -no-shutdown
```

When using the existing three-disk `qemu/Makefile` layout, use
`root=/dev/hdc3` instead.

## Milestones

### Milestone 1: Read-only mount and inode traversal

- Add ext2 on-disk headers.
- Implement superblock probe, group descriptor loading, and feature checks.
- Implement raw inode loading.
- Register the ext2 filesystem.
- Mount an ext2 partition and make the mounted filesystem root resolve to
  inode 2.
- Verify with `ls` on an empty or populated ext2 directory.

### Milestone 2: Read-only file and directory I/O

- Implement `ext2_bmap()` read path.
- Implement directory lookup and readdir.
- Implement regular file read and fast/slow symlink read.
- Implement `stat`/`statfs` integration.
- Verify with `ls -l`, `cat`, `readlink`, `find`, and `getcwd` on an ext2
  filesystem.

### Milestone 3: Write support

- Implement `ext2_write_inode()` and `ext2_write_super()`.
- Implement block and inode allocation/free.
- Implement file write and truncate.
- Implement create, unlink, mkdir, rmdir, mknod, and symlink.
- Verify with `touch`, `echo >`, `mkdir`, `rm`, `ln -s`, `mknod`, `cp`, and
  `truncate` inside a mounted ext2 filesystem.

### Milestone 4: Remaining namespace operations and hardening

- Implement `rename` and directory link-count correctness.
- Add hard-link support if the VFS gains a `link` syscall path.
- Add a small ext2 logical block cache.
- Update `/proc/mounts` and mount/umount flush behavior.
- Add corruption and boundary checks.

### Milestone 5: Direct ext2 root

- Parse `root=` and `init=` from the Multiboot command line.
- Make `ROOT_DEV` configurable while preserving the ramdisk default.
- Register ext2 before `mount_root()` and make root mount selection
  deterministic.
- Boot an ext2 image as `/` without an initrd pivot.
- Verify the ext2 root's init starts and its `/dev` nodes work.

## Validation

After the hard-disk driver exists, test with an existing ext2 partition. The
GPT partitions on `hd3.img` are the simplest dedicated targets:

```text
mkdir /mnt
mount -t ext2 /dev/hdc3 /mnt
ls -la /mnt
df /mnt
```

The MBR ext2 partitions `/dev/hda5`, `/dev/hdb1`, and `/dev/hdb6` are
alternatives for the same mount/read tests.

For populated read-only tests, populate the ext2 image on the host:

```sh
mkdir -p mnt
sudo mount -o loop ext2.img mnt
sudo sh -c 'echo hello > mnt/hello.txt; ln -s hello.txt mnt/link'
sudo umount mnt
```

Then boot with that image exposed as an IDE disk and mount it.

Write tests should use `qemu-system-i386 -snapshot` or a separate disposable
disk image. Do not write to the checked-in `qemu/hd.img` in normal tests.

Automated test candidates:

- `tests/shell/ext2.test`: mount, list, read, `stat`, and unmount.
- `tests/shell/ext2-write.test`: create/write/read/delete/rename on a
  disposable ext2 image.
- A small `app/test/ext2` program for deterministic sector/inode inspection.

## Deferred

- ext3/ext4 journaling, extents, and 64-bit block numbers.
- Htree/`DIR_INDEX` directories.
- Extended attributes, ACLs, and metadata checksums.
- Sparse superblock and resize-inode aware allocation.
- Block-group preallocation and reservation heuristics.
- Better ext2 block cache integration with the shared buffer cache.
- Mount flags and read-only remount support.

## Risks

- The ext2 superblock is not at sector 0; probing the wrong offset is a common
  first bug.
- `bread()` cannot be used directly for 1/2/4 KiB blocks, so ext2-specific
  block I/O must be careful to use the correct sector multiplier.
- Directory `f_pos` handling is easy to get wrong with deleted entries and
  large `getdents` buffers.
- Feature validation must reject ext4 images before misinterpreting extents or
  group descriptors.
- Writes must keep superblock, group descriptor, bitmap, inode, and data-block
  counts consistent, especially on error paths.
