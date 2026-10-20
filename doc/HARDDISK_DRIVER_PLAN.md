# Hard Disk Driver Plan

## Goal

Add a kernel block-device driver for the IDE/PATA hard disk used by the
standard QEMU boot path (`make run`), expose the whole disk and its DOS/MBR
partitions as `/dev/hd*`, and make them usable through the existing block-device
and buffer-cache APIs.

The driver should:

- Work with the current QEMU command in `qemu/Makefile`:
  `-drive file=hd.img,format=raw,index=0,media=disk`.
- Not disturb the existing ramdisk-based MarioFS root filesystem.
- Work in both full-disk boot (`boot.bin`/GRUB path) and the `-kernel` path,
  although `quick-run` has no disk and must continue to boot without hanging.
- Start with read-only-safe raw sector I/O, then add partition device nodes.
- Eventually allow a MarioFS image on a hard-disk partition to be mounted with
  the existing `sys_mount()` path.

## Current State

The kernel already has a simple block-device abstraction:

- `src/include/fs/blkdev.h` defines `struct blkdev_operations` with
  `blkdev_read`, `blkdev_write`, and `blkdev_get_info`.
- `src/fs/block/blkdev.c` maintains a per-major operation table and dispatches
  through `blkdev_read()`/`blkdev_write()`.
- `src/fs/block/rd.c` registers ramdisk major `RD_MAJOR (1)` and copies sectors
  directly from the Multiboot ramdisk modules.
- `src/fs/block/buffer.c` provides the synchronous buffer cache and always uses
  512-byte sectors.
- `src/fs/mariofs/` consumes block devices through `bread()`/`brelse()`.

What is missing:

- No hardware disk driver.
- No partition support in a block driver.
- Raw block device file operations are effectively absent:
  `src/fs/block/file.c` defines `blkdev_fops` with all `NULL` operations, so
  `read()`/`write()` on `/dev/rd0` or a future `/dev/hda` returns `-EINVAL`.
- The custom boot loader does not currently populate Multiboot `boot_device`.

## Driver Choice

Use a **legacy ATA PIO driver**, not AHCI/SATA or PCI IDE bus mastering.

Reasons:

- The current QEMU target is `qemu-system-i386` with the default i440FX IDE
  controller and `-drive ...,media=disk`. That presents the disk as an IDE
  device on the legacy I/O ports.
- PIO is small, deterministic, and sufficient for a first driver.
- The kernel has no PCI bus enumeration and no DMA API yet.
- IRQ-driven ATA can be added later; the first implementation can poll.

Target only 28-bit LBA initially. `qemu/gen_image.sh` creates a 400 MiB image,
which is far below the 128 GiB 28-bit LBA limit. LBA48 can be deferred.

## Architecture

### New Files

- `src/fs/block/hd.c`
  - ATA detection, PIO transfer, partition parsing, and block-device
    registration.
- `src/include/fs/hd.h`
  - Device major, command/status constants, and exported init declaration.
- Optionally extend `src/kernel/io.c` and `src/include/io.h` with
  `insl()`/`outsl()` string I/O helpers. The first version can use a
  `for` loop of `inl()`/`outl()` if string I/O is not desired yet.

`src/Makefile` automatically compiles new `*/*.c` files, so no build-list
change is required.

### Block Device Major and Nodes

Use Linux's traditional IDE disk major:

```c
#define HD_MAJOR 3
```

Device nodes are static in `rd/device_list`:

```text
hda  B 3 0
hda1 B 3 1
hda2 B 3 2
hda3 B 3 3
hda4 B 3 4
hda5 B 3 5
hda6 B 3 6
...
```

The exact partition list depends on how many MBR and EBR entries are parsed.
Keep `HD_MAJOR` below `MAX_BLKDEV`; the existing limit is 64.

Update `doc/devices.txt` after the major assignment is implemented.

## ATA PIO Design

### Ports

Primary controller:

- Command block: `0x1f0`-`0x1f7`
- Control/alternate status: `0x3f6`

Register offsets from the command base:

| Offset | Function |
|---|---|
| 0 | data |
| 1 | error/features |
| 2 | sector count |
| 3 | LBA low |
| 4 | LBA mid |
| 5 | LBA high |
| 6 | drive/head |
| 7 | status/command |

Status bits:

- `BSY 0x80`
- `DRDY 0x40`
- `DRQ 0x08`
- `ERR 0x01`

Commands:

- `IDENTIFY_DEVICE 0xec`
- `READ_SECTORS 0x20`
- `WRITE_SECTORS 0x30`
- `CACHE_FLUSH 0xe7`

### Polling Rules

The first driver should use PIO polling with ATA interrupts disabled:

1. Wait until `BSY` clears.
2. Select drive by writing the drive/head register. Use bit 6 (`0xe0`) for
   LBA mode and bit 4 for the slave.
3. Write `SECTOR_COUNT`, `LBA_LOW`, `LBA_MID`, `LBA_HIGH`.
4. Write the command.
5. Read the status register once after a short delay (for example four
   `inb(0x3f6)` reads) to allow the device to clear `DRQ`/set busy correctly.
6. Wait for `BSY` to clear and `DRQ` to set. If `ERR` is set, read the error
   register and return `-EIO`.

Use the control register bit `nIEN` (`0x02`) to prevent IRQ14/IRQ15 from firing
while polling. This avoids depending on the currently-dummy IRQ14/IRQ15 entry
paths in `src/kernel/entry.S` during the first implementation.

### Read/Write Transfers

- Read: after `DRQ` is set, read one sector's worth of data through the data
  port. For 512-byte sectors this is 128 `inl()` calls or one `rep insl`.
- Write: wait for `DRQ`, then write 512 bytes through the data port.
- Support only one sector per command initially, or allow up to 256 sectors if
  the PIO loop is written carefully. The existing buffer cache only ever asks
  for one sector at a time, so single-sector correctness is the first target.
- Validate that the requested `sector + nr` does not exceed the device size.
- Poll for completion after the final sector.

### Identify and Device Size

`hd_identify()` sends `IDENTIFY_DEVICE` and reads the 256-word identify block.
The driver must:

- Treat status `0x00` as "no device" rather than waiting forever.
- Verify the device supports LBA.
- Read words 60/61 as the 28-bit LBA sector count.
- Record model/serial bytes only for boot logging.
- If the disk reports LBA48, keep using LBA28 for now but clamp/validate
  against the 28-bit size; LBA48 support is deferred.

### Graceful No-Disk Path

`quick-run` uses only `-kernel`/`-initrd` and has no disk. `hd_init()` must:

- Probe primary master and slave without hanging.
- If no drive is present, skip `register_blkdev(HD_MAJOR, ...)` and return.
- Never change `ROOT_DEV`; ramdisk remains the root device.

This means `blkdev_init()` should call `hd_init()` after `rd_init()` but must
not require a hard disk.

## Partition Design

### Internal Table

Add an internal fixed-size partition table in `hd.c`:

```c
struct hd_partition {
    uint32_t start_sector;
    uint32_t nr_sectors;
    uint8_t present;
};
```

Minor 0 means the whole disk. Minors 1 and up map to parsed partitions.

`hd_get_info()` reports:

- minor 0: total sectors from ATA identify.
- minor > 0: sectors from the partition table, or `-ENOENT` if not present.

`hd_read()`/`hd_write()`:

- minor 0: use the LBA directly.
- minor > 0: translate to `partition.start_sector + sector`, then range-check
  against `partition.nr_sectors`.

### MBR Parsing

At init, after a successful identify:

1. Use the raw ATA PIO path, not `bread()`, because `buffer_init()` runs later
   and `hd_init()` must not recurse through the buffer cache.
2. Read sector 0 into a static 512-byte buffer.
3. Check the boot signature `0xaa55` at offset 510.
4. Walk the four 16-byte MBR partition entries at offset 446.
5. Record entries with a nonzero type. Use the LBA start at entry offset 8 and
   sector count at offset 12.

### EBR Parsing

The generated `hd.img` uses an extended partition containing logical
partitions. Add a simple EBR walk:

- The first entry in an EBR describes a logical partition.
- The second entry points to the next EBR.
- Stop when the next-ebr start is zero or a safety iteration limit is reached.

This lets `/dev/hda5`, `/dev/hda6`, and similar nodes work with the existing
`gen_image.sh` layout.

If EBR support is postponed, document that only primary partitions are
available and do not create logical partition nodes.

## Raw Block Device File Operations

`src/fs/block/file.c` should provide real `blkdev_fops` so userspace can test
with `dd`, `cmp`, `hexdump`, and eventually mount:

- `blkdev_file_lseek`:
  - Validate against the byte size from `blkdev_get_info()`.
  - Support `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`.
- `blkdev_file_read`/`blkdev_file_write`:
  - Require `f_pos` and `count` to be 512-byte aligned in the first version.
  - Convert bytes to sectors and call `blkdev_read()`/`blkdev_write()`.
  - Return `-EINVAL` for unaligned access; a later bounce-buffer version can
    remove that restriction.
- `blkdev_file_ioctl`:
  - Add block ioctls such as `BLKGETSIZE`, `BLKGETSIZE64`, and `BLKSSZGET`
    to `src/include/fs/ioctl.h` if needed for toybox/coreutils compatibility.

This applies to all block devices, including ramdisks, so it should be
implemented independently of the ATA driver and tested against `/dev/rd0`
first where possible.

## Initialization Order

Current boot order in `src/kernel/mario.c`:

```text
blkdev_init()
buffer_init()
chrdev_init()
fs_init()
```

Change `blkdev_init()` from:

```c
rd_init();
ROOT_DEV = MKDEV(RD_MAJOR, 0);
```

to:

```c
rd_init();
ROOT_DEV = MKDEV(RD_MAJOR, 0);
hd_init();   /* optional: registers HD_MAJOR only if a disk is present */
```

`hd_init()` reads the MBR using its own ATA PIO helper, before `buffer_init()`.
It must not call `bread()`.

## Files to Change

### New

- `src/fs/block/hd.c`
- `src/include/fs/hd.h`

### Modified

- `src/fs/block/blkdev.c`
  - Call `hd_init()` after `rd_init()`.
- `src/include/fs/blkdev.h`
  - Add `HD_MAJOR`.
- `src/fs/block/file.c`
  - Implement raw block `read`/`write`/`lseek`/`ioctl`.
- `src/include/fs/ioctl.h`
  - Add block-size ioctl definitions if desired.
- `rd/device_list`
  - Add `hda`, `hda1`, etc.
- `doc/devices.txt`
  - Document major 3 and the new nodes.
- Optionally `src/kernel/io.c` / `src/include/io.h`
  - Add `insl()`/`outsl()` helpers.

No change is needed to `src/Makefile` for the new C file.

## Milestones

### Milestone 1: ATA PIO whole-disk driver

- Add ATA register/command constants.
- Implement `ata_wait_not_busy()`, `ata_select_drive()`, `ata_pio_read_sectors()`,
  `ata_pio_write_sectors()`.
- Implement `hd_identify()` with a timeout/no-device path.
- Register major `HD_MAJOR`, minor 0 only.
- Add `/dev/hda` to `rd/device_list`.
- Implement raw block fops so `dd if=/dev/hda ... count=1` works.

### Milestone 2: MBR and EBR partition nodes

- Parse MBR sector 0.
- Build the partition table for minors 1-4.
- Add EBR walking for logical minors.
- Add partition nodes to `rd/device_list`.
- Range-check reads/writes against partition sizes.

### Milestone 3: Write safety and flush

- Add PIO write path.
- Add `CACHE_FLUSH` on clean shutdown/reboot if/when a shutdown hook exists.
- Add a dedicated write-test disk or use `-snapshot` to avoid corrupting
  `qemu/hd.img`.

### Milestone 4: Block-device userspace integration

- Test raw sector reads against known values in `hd.img`.
- Format or write a MarioFS image to a test partition and mount it with
  `mount -t mariofs /dev/hdaN /mnt`, proving the block stack works end to end.
- Add a small block-device test program under `app/test/` if shell-only tests
  are insufficient.

### Deferred

- LBA48 for disks above 128 GiB.
- Secondary ATA controller.
- ATAPI/PACKET for CD-ROM devices.
- PCI IDE bus mastering and UDMA.
- IRQ-driven ATA completion with `sleep_on()`/`wake_up()`.
- Byte-granular raw block reads/writes via a bounce buffer.
- Dynamic device nodes and `/proc/partitions`.
- Booting directly from the hard disk instead of ramdisk root.

## Validation

Manual checks:

```sh
make kernel rd
make -C qemu run
```

In the shell:

```text
ls -l /dev/hda /dev/hda1 /dev/hda5
dd if=/dev/hda of=/root/mbr.bin bs=512 count=1
hexdump -C -n 64 /root/mbr.bin
```

Expected:

- The first sector begins with the x86 boot code/MBR and ends with `55 aa`.
- `blkdev_get_info()` reports the same total sectors as `sfdisk -l qemu/hd.img`.
- Partition nodes reject reads beyond their partition length.
- `make quick-run` still boots with no disk.

Read-only tests should use the normal boot disk. Write tests should use
`qemu-system-i386 -snapshot` or a separate disposable disk image so the checked
in `qemu/hd.img` is not modified.

Automated test ideas:

- Add `tests/shell/hd.test` using `dd`/`hexdump` to read the MBR signature.
- Add a small `app/test/hd` program that opens `/dev/hda`, checks geometry,
  reads sector 0, and prints a stable checksum.
- Keep disk write coverage in a separate QEMU invocation or `-snapshot` mode.

## Risks

- IDE floating bus/no device detection can return `0xff` and look like an
  always-busy device. `hd_identify()` needs explicit status-`0x00` handling
  and bounded polling loops.
- Polling must not hang the kernel forever on real hardware. Use a fixed
  retry/timeout count.
- Partition parsing must validate sector offsets before translating reads.
- `quick-run` must remain usable; `hd_init()` must be optional.
- Raw block fops affect ramdisks too. Test `/dev/rd0` before `/dev/hda` to
  isolate regressions.
