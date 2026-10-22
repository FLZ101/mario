# Proc Filesystem Plan

## Goal

Turn `/proc` from a synthetic path hook into a real registered filesystem
mounted at `/proc`. After this change:

- `ls /` shows `proc`.
- `/proc` appears in `mount` and `/proc/mounts`.
- `/proc` can be mounted and unmounted through the normal mount syscall.
- Proc inodes belong to a superblock instead of floating with `i_sb == NULL`.

## Current State

Procfs already has most of the filesystem object implementation in
`src/fs/proc.c`:

- `proc_dir_iops` provides lookup and readdir.
- `/proc/self`, `/proc/version`, `/proc/uptime`, `/proc/mounts`,
  `/proc/partitions`, and per-process entries already work.
- `proc_get_root()` allocates a synthetic root inode on demand.

But procfs is not a real filesystem:

- There is no `struct file_system_type` for proc.
- There is no proc superblock in `src/fs/super_block.c`.
- `src/fs/namei.c:96` special-cases the string `"proc"` when looking it up
  from the root of the root filesystem.
- Root readdir is MarioFS readdir, so it never emits the virtual `proc` name.
- `proc_alloc_inode()` in `src/fs/proc.c:153` leaves `inode->i_sb = NULL`.
- `sys_mount()` only supports block-device-backed filesystems.
- The root ramdisk has no `/proc` directory, and userspace init never mounts
  proc.

## Design

### 1. Add a nodev filesystem flag

Extend `struct file_system_type` in `src/include/fs/fs.h`:

```c
#define FS_NODEV		1

struct file_system_type {
	struct super_block *(*read_super)(struct super_block *);
	struct list_head list;
	char *name;
	int fs_flags;
};
```

Existing filesystem initializers leave `fs_flags` as zero, meaning the normal
block-device path. Proc sets `FS_NODEV`.

### 2. Teach superblock management about nodev filesystems

`src/fs/super_block.c` currently keys superblocks by `s_dev`, and
`get_super(dev)` rejects `dev == 0`. Add a type-based lookup for nodev
filesystems:

```c
static struct super_block *get_super_by_type(struct file_system_type *fs);
```

`read_super(dev, name)` becomes:

1. If `dev != 0`, keep the existing `get_super(dev)` fast path.
2. Resolve `struct file_system_type *fs`.
3. If `fs->fs_flags & FS_NODEV`, look for an existing superblock with the
   same `s_fsname` and return it.
4. Otherwise allocate the next free superblock and set `s_dev = dev` as today.

`mount_root()` must skip `FS_NODEV` filesystems so it does not try to read proc
from `ROOT_DEV`.

`sys_mount()` branches on `fs_type->fs_flags & FS_NODEV`:

- Normal filesystems keep the current `namei(dev_name)` + block-device checks.
- Nodev filesystems skip device lookup and call the common mount-attach code
  with `dev = 0`.

Refactor the mount/unmount common paths:

- Change `do_mount()` to take `struct file_system_type *fs_type` and skip
  `fs_may_mount()` for `FS_NODEV`.
- Introduce `do_umount_sb(struct super_block *sb)`.
- `sys_umount()` uses `do_umount_sb()` for directory mountpoints and keeps the
  device lookup for block-device paths.
- Protect the root mount by checking `sb->s_covered == NULL`, not by comparing
  `s_dev` with `ROOT_DEV`.

Finally, `get_mount_super()` and `get_nr_mounts()` should test
`s_mounted && s_fsname` instead of `s_dev && s_mounted`; otherwise nodev
mounts are invisible to `/proc/mounts`.

### 3. Add a proc superblock

Add `PROC_SUPER_MAGIC` and a `proc` `struct file_system_type` in procfs.

```c
struct super_operations proc_sops = {
	/* no on-disk read/write callbacks */
};

static struct super_block *proc_read_super(struct super_block *sb)
{
	sb->s_magic = PROC_SUPER_MAGIC;
	sb->s_block_size = 512;
	sb->s_op = &proc_sops;
	sb->s_mounted = proc_make_dir(PROC_INO_ROOT);
	return sb->s_mounted ? sb : NULL;
}

struct file_system_type proc = {
	.read_super = proc_read_super,
	.list = LIST_HEAD_INIT(proc.list),
	.name = "proc",
	.fs_flags = FS_NODEV,
};
```

Proc inode constructors need to attach inodes to the proc superblock:

- Add a `struct super_block *sb` argument to `proc_alloc_inode()`, or keep a
  file-scope `static struct super_block *proc_sb` set by `proc_read_super()`.
- `proc_make_file()`, `proc_make_link()`, and `proc_make_dir()` pass the active
  proc superblock down to `proc_alloc_inode()`.
- `proc_alloc_inode()` sets `inode->i_sb = sb` and `inode->i_dev = sb->s_dev`.
- `proc_get_root()` returns `proc_sb->s_mounted` with a reference rather than
  allocating a fresh root inode each call.
- `proc_parent(PROC_INO_ROOT)` must return the covered mountpoint inode
  (`proc_sb->s_covered`) with a reference, not `current->fs->root`.

Register proc next to the other filesystems in `src/fs/fs.c`.

### 4. Create and mount `/proc`

Add a real mountpoint directory to the generated ramdisk:

```sh
# rd/make-rd0.sh
mkdir -p ramdisk0/{bin/example,bin/test,mnt,proc,root/work,usr/share/terminfo}
```

Mount proc from init before spawning gettys:

```c
/* app/example/init/main.c */
if (mount("proc", "/proc", "proc", 0, NULL))
	exit(EXIT_FAILURE);
```

This keeps filesystem setup in userspace, like a conventional init.

After proc is mounted on a real `/proc` directory, the covering MarioFS inode
has `i_mount` set to the proc root. The existing `iget(crossmnt)` path crosses
the mountpoint automatically, so remove the hardcoded `"proc"` special case
from `src/fs/namei.c`.

Keep this proc-internal routing in `lookup()`:

```c
if (dir->i_op == &proc_dir_iops)
	return dir->i_op->lookup(dir, name, len, res);
```

That is required to stay inside proc after crossing into its root.

### 5. Fix `/proc/mounts` reporting for proc

`proc_mount_source()` currently assumes a non-root mount is a block device.
Add a nodev branch:

```c
if (!sb->s_dev && sb->s_fsname)
	return sprintk(buf, "%s", sb->s_fsname);
```

Then proc appears as:

```text
proc /proc proc rw 0 0
```

### 6. Optional statfs support

Add a `PROC_SUPER_MAGIC` branch to `statfs64_from_inode()` in
`src/fs/stat.c`. Without it, `statfs("/proc")` and `statfs64` on proc inodes
return `-ENOSYS`. A minimal proc branch can report:

```c
f_type = PROC_SUPER_MAGIC;
f_bsize = f_frsize = 512;
f_blocks = f_bfree = f_bavail = 0;
f_namelen = NAME_MAX;
```

## Implementation Order

1. Add `FS_NODEV` and the proc superblock declaration in
   `src/include/fs/fs.h`/proc headers.
2. Add nodev superblock lookup/mount/unmount support in
   `src/fs/super_block.c`.
3. Add `proc_read_super()`, `struct file_system_type proc`, and inode
   superblock attachment in `src/fs/proc.c`.
4. Register proc in `src/fs/fs.c`.
5. Remove the root `"proc"` lookup special case from `src/fs/namei.c`.
6. Add the generated `/proc` mountpoint in `rd/make-rd0.sh`.
7. Mount proc in `app/example/init/main.c`.
8. Update `proc_mount_source()` for nodev mounts.
9. Add the proc statfs branch.
10. Extend shell tests and run `make test`.

## Testing

Existing tests:

- `tests/shell/proc.test`
- `tests/shell/toybox.mount.test`

Add or update checks:

```text
RUN: ls /
CHECK: proc

RUN: cat /proc/version
CHECK: Mario OS version 1.0

RUN: mount
CHECK: proc on /proc type proc (rw)

RUN: cat /proc/mounts
CHECK: proc /proc proc rw 0 0

RUN: test -d /proc/self/fd
```

Also verify getcwd and path traversal still work from `/proc`:

```text
RUN: cd /proc/self/fd && pwd
```

## Risks and Notes

- `s_dev == 0` is shared by all future nodev filesystems. The single-proc
  version is safe, but `/proc/mounts` enumeration and unmount busy checks may
  need to become superblock-based if more nodev filesystems are added.
- Proc inodes are allocated with `get_empty_inode()` and are not in the inode
  hash. This is fine as long as every proc inode constructor attaches the same
  proc superblock.
- The ramdisk `/proc` directory is generated by `rd/make-rd0.sh`. If needed,
  add the script itself to the `rd0` make dependencies so editing it rebuilds
  the image.
- Keep the `FS_NODEV` branch out of `mount_root()` to avoid probing proc on
  `ROOT_DEV`.
