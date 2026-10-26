# Ramdisk-to-ext2 Root Switch Plan

## Goal

Boot Mario from a deliberately small ramdisk and make an ext2 partition the
permanent writable filesystem. The first target is direct ext2 root:

```text
kernel -> mount /dev/hdc3 as / -> exec /sbin/init from ext2
```

The ramdisk becomes a tiny recovery/bootstrap image rather than the place where
Python, vim, ncurses, fonts, and the GUI live.

## Current State

- `rd/make-rd0.sh` builds a 40 MiB ramdisk from `rd/_ramdisk0` plus almost all
  userspace binaries. It calls `rd-make.exe --nr-block=81920`.
- `src/fs/block/blkdev.c` hardcodes `ROOT_DEV = MKDEV(RD_MAJOR, 0)`, so the
  ramdisk is always the root filesystem.
- `src/kernel/mario.c` hardcodes the first userspace process as
  `/bin/example/init.exe`.
- `src/fs/fs.c` already registers both MarioFS and ext2 before `mount_root()`.
- The ext2 driver in `src/fs/ext2/` already has read/write/inode/dir support.
- `sys_chroot()` exists, but `SYS_pivot_root` is still `sys_not_exist`.

## Scope

In scope:

- A minimal ramdisk build profile for recovery and early boot.
- Kernel `root=`, `init=`, and optional `console=` command-line parsing.
- Booting directly with an ext2 partition as `/`.
- Populating that ext2 partition with `/sbin/init`, `/bin`, `/etc`, `/usr`,
  and static `/dev` nodes.
- Moving large userspace payloads from `rd0` to the ext2 partition.
- A userspace `chroot`-based switch as the fallback for the current VFS.
- Boot and filesystem tests for both ramdisk-root and ext2-root modes.

Out of scope:

- `pivot_root` in the first milestone; use direct ext2 root for real memory
  savings, and treat `chroot` switching as an interim fallback only.
- devtmpfs, tmpfs, bind mounts, overlayfs, or dynamically loaded kernel
  modules.
- Network boot or initramfs compression.

## Design

### Option A: direct ext2 root (recommended)

This is the memory-efficient option. The kernel sets `ROOT_DEV` to
`/dev/hdc3`, `mount_root()` reads the ext2 superblock, and init is exec'd from
that filesystem. The ramdisk module may still be loaded, but it is not
mounted as `/` and can be kept tiny or omitted for production boot.

Boot line with the current three-disk QEMU layout:

```text
root=/dev/hdc3 init=/sbin/init console=/dev/ttyS0
```

Boot line if `hd3.img` is attached as the only AHCI disk:

```text
root=/dev/hda3 init=/sbin/init console=/dev/ttyS0
```

The ext2 image must be self-contained: init, shell, mount, basic tools,
libraries, configuration, and device nodes all live on the ext2 partition.

### Option B: small ramdisk bootstrap, then switch root

This keeps `ROOT_DEV=rd0`, starts a minimal `/bin/init`, mounts ext2 under
`/newroot`, and switches the current process into it:

```sh
mount -t ext2 /dev/hdc3 /newroot
cd /newroot
chroot . /sbin/init
```

This works with the existing `sys_chroot()`, but it does not free the old
ramdisk root from memory. Unmounting it is currently a no-op for `ROOT_DEV`,
and `pivot_root` is not implemented. Use this only for staged migration or
recovery until direct ext2 root is ready.

### Small ramdisk contents

The small `rd0` profile keeps only what is needed before the ext2 mount:

```text
/bin/init
/bin/oksh
/bin/mount
/bin/toybox        # optional multi-call tools: ls, mkdir, sleep, cat
/etc/profile
/newroot
/dev/console
/dev/null
/dev/ttyS0
/dev/hdc3
/dev/rd0
```

Target size:

- `rd0` under 8 MiB (`--nr-block=16384` with 512-byte MarioFS blocks).
- Full userspace, Python, vim, ncurses, terminfo, fonts, and GUI files move to
  the ext2 partition.
- Keep `rd1`/`rd2` unchanged as static smoke-test modules; they are not needed
  for root switching.

## File Changes

Kernel:

```text
src/include/multiboot.h
src/kernel/mario.c
src/fs/block/blkdev.c
src/include/fs/blkdev.h
src/fs/fs.c              # no logic change expected; keep ext2 registration early
src/fs/open.c            # only if hardening chroot/pivot semantics later
src/kernel/entry.S       # only if implementing pivot_root later
```

Build and image:

```text
Makefile
rd/Makefile
rd/make-rd0.sh           # add RD_PROFILE=full|small branch
rd/make-rd0-small.sh     # new minimal profile
rd/device_list
qemu/Makefile
qemu/make-hd3.py
```

Userspace / test:

```text
app/init/ or app/usr/sbin/init
rd/etc/passwd
rd/etc/profile
tests/shell/rootdev.test
tests/shell/ext2.root.test
tests/shell/ext2.persist.test
```

## Milestones

### Milestone 0: small ramdisk profile

Goal: produce a bootable 8 MiB or smaller `rd0` without breaking the existing
full-profile build.

Steps:

1. Add `RD_PROFILE ?= full` to `Makefile` and `rd/Makefile`.
2. Refactor `rd/make-rd0.sh` so `--deps` and profile selection are explicit.
3. Add `rd/make-rd0-small.sh`:
   - copy only init, shell, mount, device nodes, and `/newroot`;
   - call `rd-make.exe --dir=ramdisk0 --rd=rd0 --device-list=device_list
     --nr-block=16384`.
4. Keep the existing full profile as the default for current shell/VGA tests.
5. Add a size check target:

```sh
make RD_PROFILE=small rd
du -b rd/rd0
```

Checkpoint:

- `make test` still passes with `RD_PROFILE=full`.
- `RD_PROFILE=small` boots to the existing init and prints the startup
  banner; missing full-size tools are expected and documented.

### Milestone 1: parse root/init/console from the Multiboot command line

Goal: make the kernel choose `ROOT_DEV` and the init path from boot
parameters instead of fixed constants.

Steps:

1. Convert `m->cmdline` from the physical Multiboot address to a usable
   kernel string once low memory is still mapped, or copy it into a static
   buffer in `setup_memory_region()`.
2. Add `parse_root_device(const char *cmdline)`:

```c
root=/dev/rd0
root=/dev/hdc3
root=/dev/hda3
```

3. Map device names to `MKDEV(MAJOR, minor)` using the existing disk minor
   layout:

```c
disk_minor = disk_index * HD_MINORS_PER_DISK;
partition_minor = disk_minor + partition_number;
```

4. Add `init=` parsing and store the path in a kernel string, defaulting to
   `/bin/example/init.exe` for compatibility.
5. Move `ROOT_DEV` assignment out of `blkdev_init()`:

```c
void blkdev_init(struct multiboot_info *m)
{
    rd_init();
    setup_root_device(m);
    hd_init();
}
```

6. Pass `m` from `mario()` into `blkdev_init()`.
7. Add `tests/shell/rootdev.test` that boots with `root=/dev/rd0` and then
   `root=/dev/hdc3`; verify `/proc/mounts` or `/dev/root` reflects the
   selected device.

Checkpoint:

- `root=/dev/rd0` behavior is identical to today.
- Unknown or missing `root=` falls back to ramdisk.
- `root=/dev/hdc3` reaches ext2 read_super and does not hang.

### Milestone 2: direct ext2 root boot

Goal: run `/sbin/init` from `/dev/hdc3` with no initrd-mounted root.

Steps:

1. Ensure `mount_root()` accepts ext2 on `ROOT_DEV`; it already probes all
   registered filesystems, so the main requirement is correct `ROOT_DEV`.
2. Change init launching in `src/kernel/mario.c`:

```c
execve(kernel_init_path, argv_init, envp_init);
```

3. Populate `qemu/hd3/p3` with a real root filesystem in
   `qemu/make-hd3.py`:

```text
/sbin/init
/bin/oksh
/bin/toybox
/bin/mount
/etc/profile
/etc/passwd
/usr/bin/...
/dev/console
/dev/null
/dev/ttyS0
/dev/hdc3
```

4. Create the device nodes before packaging the ext2 image. Use static nodes;
   do not rely on a devtmpfs that Mario does not have.
5. Add `qemu/Makefile` variable:

```make
ROOT_ARGS ?= root=/dev/hdc3 init=/sbin/init console=/dev/ttyS0
```

and append `$(ROOT_ARGS)` to `QUICK_QEMU_FLAGS`.
6. Add a `quick-ext2-root` target that starts QEMU with `hd3.img` and those
   args.
7. Verify `/proc/mounts` shows ext2 on `/` and `getcwd()` resolves from the
   ext2 inode.

Checkpoint:

- Booting with `root=/dev/hdc3` starts `/sbin/init` from ext2.
- Writing a file in ext2 `/tmp`, rebooting, and reading it back works.
- Booting with `root=/dev/rd0` remains unchanged.

### Milestone 3: install full userspace on ext2

Goal: move the large read-only payload out of `rd0`.

Steps:

1. Keep small runtime files in `rd0`: init, shell, mount, minimal device
   nodes, `/etc/profile`.
2. Put Python, vim, nano, less, ncurses programs, terminfo, docs, and fonts
   under ext2 `/usr`.
3. Symlink from ext2 `/bin` and `/usr/bin` into the corresponding tools.
4. Do not copy example/test executables into either root unless a test
   explicitly requires them; keep them in `rd1`/`rd2` or `/usr/test`.
5. Update `qemu/make-hd3.py` content mapping and `rd/make-rd0-small.sh`
   exclusions.
6. Add an ext2 package/layout test:

```sh
ls /usr/bin/python3.13
python3 -c 'print("ext2 usr ok")'
ls /usr/share/terminfo/x/xterm
```

Checkpoint:

- `RD_PROFILE=small` boots from ext2 with all full-size tools available.
- `du -b rd/rd0` stays below the 8 MiB budget.

### Milestone 4: userspace chroot switch fallback

Goal: support the staged migration mode for environments that still want an
initrd root initially.

Steps:

1. Add `/newroot` to the small ramdisk.
2. Add a tiny `/bin/init` mode that performs:

```sh
mount -t ext2 /dev/hdc3 /newroot
cd /newroot
chroot . /sbin/init
```

3. Ensure ext2 root has the same static `/dev` nodes as the direct-root path.
4. Document that the old ramdisk root stays mounted and consumes memory.
5. Add a test that verifies `/proc/mounts` reports both ramdisk root and ext2
   `/newroot` after the switch.

Checkpoint:

- The fallback switch works.
- It is used only for migration/recovery; direct ext2 root remains the
  production path.

### Milestone 5: pivot_root and freeing the old root (optional hardening)

Goal: replace the `chroot` fallback with a real pivot if ramdisk memory
reclamation is required.

Steps:

1. Implement `sys_pivot_root()`.
2. Teach `sys_umount()` that the old root may be unmounted after pivot while
   keeping the new root protected.
3. Release the ramdisk pages after successful switch only if the block device
   is no longer referenced.
4. Add `pivot_root` tests that boot from ramdisk and end with `/` on ext2.

Checkpoint:

- `/proc/mounts` no longer shows the ramdisk after the switch.
- Memory after switch is closer to the direct ext2-root case.

## Validation

Before changes:

```sh
make shell-test
make vga-test
```

Root-device tests:

```sh
make ROOT_ARGS="root=/dev/rd0" quick-run
make ROOT_ARGS="root=/dev/hdc3 init=/sbin/init console=/dev/ttyS0" quick-run
```

Small-rd build:

```sh
make RD_PROFILE=small image
du -b rd/rd0
```

Ext2 root tests:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell/rootdev.test
.venv/bin/python tests/lit_shell_test.py tests/shell/ext2.root.test
.venv/bin/python tests/lit_shell_test.py tests/shell/ext2.persist.test
```

Manual verification:

1. Boot with `root=/dev/hdc3`.
2. Confirm `/sbin/init` came from ext2 and `getcwd()` is `/`.
3. Run `python3`, `vim`, `less`, and `mount`.
4. Create `/root/persist.txt`, reboot, and confirm it still exists.
5. Boot with `root=/dev/rd0` and confirm the old path still works.
6. Build `RD_PROFILE=small` and confirm `rd/rd0` is under 8 MiB.

## Risks and Rollback

- The ext2 image must contain a valid init and static device nodes. If init
  is missing, the kernel hangs after `execve`; keep `root=/dev/rd0` as the
  fallback boot target.
- Device names shift between the three-disk QEMU layout and a one-disk test
  layout. Keep `hdc3` for the normal layout and document `hda3` for the
  single-disk layout.
- `mount_root()` currently accepts the first filesystem whose `read_super()`
  succeeds. If ext2 probing returns false positives, constrain it to the
  explicit filesystem type in the boot parameter.
- `sys_umount()` deliberately ignores `ROOT_DEV`; do not attempt to free the
  ramdisk in Option B until `pivot_root` support lands.
- `sys_chroot()` changes only `current->fs->root`, not the working directory.
  Always `chdir("/newroot")` before `chroot(".")`.
- Keep the full-profile ramdisk default until the ext2 root tests are stable;
  flipping the default too early would break existing shell/VGA tests.

## Open Questions

- Do we keep `rd0` loaded as a rescue root for all ext2-root boots, or omit
  the initrd entirely once the ext2 image is trusted?
- Should `root=/dev/hdc3` imply `rootfstype=ext2`, or should `mount_root()`
  continue probing MarioFS then ext2?
- Is `/usr` on ext2 a bind-style replacement for the current ramdisk `/usr`,
  or should the ext2 image own `/bin`, `/etc`, and `/root` too?
