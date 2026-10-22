# Sysfs Filesystem Plan

## Goal

Turn `/sys` into a real `sysfs` filesystem while keeping `debugfs` as a
separate filesystem mounted under it at `/sys/kernel/debug`.

After this change:

- `ls /` shows `sys`.
- `/sys` is a mounted sysfs filesystem.
- `/sys/kernel/debug` is a mounted debugfs filesystem.
- Both appear in `mount` and `/proc/mounts`.
- The existing `/sys/kernel/debug/syscalls` and
  `/sys/kernel/debug/trace_pid` behavior is preserved.

## Current State

Today the two filesystems are conflated by a namei hook:

- `src/fs/namei.c:95` special-cases the path component `"sys"` from the root
  filesystem and returns `debugfs_get_root()`.
- `src/fs/debugfs.c` implements a virtual root containing:

  ```text
  kernel
    debug
      syscalls
      trace_pid
  ```

That tree is effectively acting as both sysfs and debugfs. There is no
`struct file_system_type` for either sysfs or debugfs, no superblocks, and
the root ramdisk has no `/sys` or `/sys/kernel/debug` directories.

## Design

Reuse the `FS_NODEV` support added for proc. Sysfs and debugfs are two
distinct nodev filesystems.

### 1. Split the virtual trees

Sysfs owns the mount topology:

```text
/sys
  kernel
    debug      # empty mountpoint directory in sysfs
```

Debugfs owns the trace files:

```text
<mounted at /sys/kernel/debug>
  syscalls
  trace_pid
```

Add new `src/fs/sysfs.c` and `src/include/fs/sysfs.h`. Keep the existing
`src/fs/debugfs.c`, but change its root inode to expose `syscalls` and
`trace_pid` directly instead of `kernel/debug`.

### 2. Add sysfs

Create a minimal sysfs implementation:

- `SYSFS_SUPER_MAGIC` in `src/include/fs/sysfs.h`.
- Root directory `kernel`.
- `kernel` directory containing `debug`.
- Directory lookup and readdir for both levels.
- `sysfs_get_root()` returning the mounted root inode.
- `sysfs_parent()` for the root returning the covered `/sys` inode.
- `sysfs_alloc_inode()` sets `inode->i_sb` to the active sysfs superblock.

```c
struct file_system_type sysfs = {
	.read_super = sysfs_read_super,
	.list = LIST_HEAD_INIT(sysfs.list),
	.name = "sysfs",
	.fs_flags = FS_NODEV,
};
```

### 3. Refactor debugfs to a real debugfs

Keep `debugfs` as its own filesystem type:

- Add `DEBUGFS_SUPER_MAGIC` in `src/include/fs/debugfs.h`.
- Change debugfs root readdir/lookup so the root directly lists:

  ```text
  syscalls
  trace_pid
  ```

- Add `debugfs_read_super()` and:

  ```c
  struct file_system_type debugfs = {
  	.read_super = debugfs_read_super,
  	.list = LIST_HEAD_INIT(debugfs.list),
  	.name = "debugfs",
  	.fs_flags = FS_NODEV,
  };
  ```

- Attach debugfs inodes to the active debugfs superblock.
- `debugfs_parent()` for the root returns the covered inode
  (`/sys/kernel/debug`), not `current->fs->root`.

### 4. Register both filesystems

In `src/fs/fs.c`:

```c
register_file_system(&sysfs);
register_file_system(&debugfs);
```

`mount_root()` already skips `FS_NODEV` entries, so neither is probed against
`ROOT_DEV`.

### 5. Create and mount `/sys` and `/sys/kernel/debug`

Add both mountpoint directories in `rd/make-rd0.sh`:

```sh
mkdir -p ramdisk0/sys/kernel/debug
mkdir -p ramdisk0/{bin/example,bin/test,mnt,proc,root/work,usr/share/terminfo}
```

Mount sysfs first, then debugfs under it:

```c
if (0 != mount("sysfs", "/sys", "sysfs", 0, NULL)) {
	perror("mount sysfs");
	exit(EXIT_FAILURE);
}

if (0 != mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL)) {
	perror("mount debugfs");
	exit(EXIT_FAILURE);
}
```

The order matters: the debugfs mountpoint must exist inside the mounted sysfs
tree before debugfs can be attached.

### 6. Remove the namei special case

After `/sys` is a real mountpoint, the covering MarioFS inode has `i_mount`
set to the sysfs root. Generic `iget(crossmnt)` crosses it automatically.

Remove the `"sys"` special case from `src/fs/namei.c`.

Keep the sysfs/debugfs-internal routing:

```c
if (dir->i_op == &sysfs_dir_iops)
	return dir->i_op->lookup(dir, name, len, res);
if (dir->i_op == &debugfs_dir_iops)
	return dir->i_op->lookup(dir, name, len, res);
```

### 7. Update `/proc/mounts`

The generic nodev branch added for proc already reports filesystem names for
nodev mounts:

```text
sysfs /sys sysfs rw 0 0
debugfs /sys/kernel/debug debugfs rw 0 0
```

No further `/proc/mounts` change is required.

### 8. Optional statfs support

Add branches for both `SYSFS_SUPER_MAGIC` and `DEBUGFS_SUPER_MAGIC` in
`src/fs/stat.c`, matching the proc branch:

```c
f_bsize = f_frsize = 4096;
f_blocks = f_bfree = f_bavail = 0;
f_namelen = NAME_MAX;
```

## Implementation Order

1. Add `src/include/fs/sysfs.h` and `src/fs/sysfs.c`.
2. Refactor `src/fs/debugfs.c` so its root exposes `syscalls` and `trace_pid`.
3. Add superblocks and `struct file_system_type` objects for sysfs and debugfs.
4. Register both in `src/fs/fs.c`.
5. Remove the `"sys"` namei special case and add sysfs/debugfs internal lookup
   routing if needed.
6. Add `/sys/kernel/debug` to the ramdisk image.
7. Mount sysfs and debugfs from init in order.
8. Add the optional statfs branches.
9. Update shell tests and run `make test`.

## Testing

Existing tests should continue to pass:

- `tests/shell/debugfs.syscalls.test`
- `tests/shell/python.trace.test`

Add checks for the real mounts:

```text
RUN: ls /
CHECK: sys

RUN: mount
CHECK: sysfs on /sys type sysfs (rw)
CHECK: debugfs on /sys/kernel/debug type debugfs (rw)

RUN: cat /proc/mounts
CHECK: sysfs /sys sysfs rw 0 0
CHECK: debugfs /sys/kernel/debug debugfs rw 0 0

RUN: ls /sys
CHECK: kernel
RUN: ls /sys/kernel
CHECK: debug
RUN: ls /sys/kernel/debug
CHECK: syscalls
CHECK: trace_pid

RUN: cd /sys/kernel/debug && pwd
CHECK: /sys/kernel/debug
```

Also verify unmount/remount in mount order:

```text
RUN: umount /sys/kernel/debug; echo debug-umount:$?
CHECK: debug-umount:0
RUN: umount /sys; echo sys-umount:$?
CHECK: sys-umount:0
RUN: mount -t sysfs sysfs /sys; echo sys-remount:$?
CHECK: sys-remount:0
RUN: mount -t debugfs debugfs /sys/kernel/debug; echo debug-remount:$?
CHECK: debug-remount:0
```

## Risks and Notes

- Debugfs and sysfs must remain separate `struct file_system_type` objects.
- `debugfs.c` is currently coupled to `/sys/kernel/debug`; refactoring its
  root entries from `kernel/debug` to direct trace files is required so it can
  be mounted at `/sys/kernel/debug`.
- Sysfs and debugfs are both nodev filesystems with `s_dev == 0`; the existing
  superblock allocator already distinguishes used nodev superblocks with
  `s_fsname`.
- Init must mount sysfs before debugfs because debugfs's mountpoint lives in
  sysfs.
