# Hard Disk Driver Plan

## Goal

Add a kernel AHCI/SATA block-device driver for the hard disk used by the
standard QEMU boot path (`make run`), expose the whole disk and its DOS/MBR
partitions as `/dev/hd*`, and make them usable through the existing
block-device and buffer-cache APIs.

The driver must provide:

- AHCI/SATA command submission through memory-mapped HBA registers.
- 48-bit LBA addressing for both identify and read/write commands.
- IRQ-driven command completion, not polling in the normal I/O path.
- A graceful no-controller path so `quick-run` continues to boot.
- Partition-aware device nodes (`/dev/hda`, `/dev/hda1`, `/dev/hda5`, ...).
- Real raw block file operations so `dd`, `hexdump`, `cmp`, and `mount` work.
- No disturbance to the existing ramdisk-based MarioFS root filesystem.

## Current State

The kernel already has a simple block-device abstraction:

- `src/include/fs/blkdev.h` defines `struct blkdev_operations` with
  `blkdev_read`, `blkdev_write`, and `blkdev_get_info`.
- `src/fs/block/blkdev.c` maintains a per-major operation table and dispatches
  through `blkdev_read()`/`blkdev_write()`.
- `src/fs/block/rd.c` registers ramdisk major `RD_MAJOR (1)` and copies sectors
  directly from the Multiboot ramdisk modules.
- `src/fs/block/buffer.c` provides the synchronous 512-byte buffer cache.
- `src/fs/mariofs/` consumes block devices through `bread()`/`brelse()`.

What is missing:

- No PCI bus enumeration.
- No AHCI/SATA controller driver.
- No MMIO mapping or 32/64-bit MMIO register access helpers.
- No generic IRQ registration/dispatch; IRQ handlers are hard-coded in
  `src/kernel/entry.S`.
- No 48-bit LBA command path.
- No partition support in a block driver.
- Raw block device file operations are effectively absent:
  `src/fs/block/file.c` defines `blkdev_fops` with all `NULL` operations.

## Driver Choice

Use **AHCI/SATA**, not the legacy ATA PIO controller.

Reasons:

- AHCI is the required controller class for this plan.
- AHCI is memory-mapped and supports native command queues, 64-bit PRDs, and
  48-bit LBA commands without the legacy PIO port polling model.
- The kernel already has page allocation, wait queues, and interrupt gates;
  adding PCI enumeration and MMIO access is smaller than adding legacy IDE,
  PIO polling, and later replacing it with AHCI.

The QEMU launch configuration is changed so the disk is attached to an AHCI
controller (see "QEMU Configuration").

## Architecture

### New Files

- `src/include/pci.h` and `src/kernel/pci.c`
  - PCI configuration-space access and device discovery.
  - Find an AHCI controller by class code `0x01/0x06/0x01`
    (mass storage / SATA / AHCI).
- `src/include/fs/ahci.h` and `src/fs/block/ahci.c`
  - AHCI HBA register definitions, memory structures, command submission,
    interrupt handling, identify, read, write, and partition registration.
- `src/include/mm/ioremap.h` and `src/mm/ioremap.c`
  - Map a physical MMIO region into kernel virtual space and return a
    `volatile void *` MMIO pointer.
- `src/include/irq.h` and `src/kernel/irq.c`
  - Add a small IRQ handler registration table and EOI/ack helpers.
  - Replace the current hard-coded IRQ stubs with dispatch through the table,
    or add a dedicated AHCI vector if only one controller needs support.

`src/Makefile` automatically compiles new `*/*.c` files, so no build-list
change is required.

### PCI Enumeration

- Add `pci_config_read32(bus, slot, function, offset)` and
  `pci_config_write32()` using I/O ports `0xcf8`/`0xcfc`.
- Scan bus 0 and, if bridges are found, buses up to 255.
- Match devices where:
  - class code = `0x01`
  - subclass = `0x06`
  - programming interface = `0x01`
- Read BAR5 for the AHCI ABAR (usually a 32-bit memory BAR, but support
  64-bit BARs if the low bits indicate a 64-bit address).
- Read the PCI interrupt line and use it when registering the AHCI interrupt.
- Do not assume the AHCI controller has a specific vendor/device ID; the
  class code is the primary match.

### MMIO Mapping

The current page tables map RAM using `KERNEL_BASE + physical_address`.
PCI MMIO BARs are not necessarily part of the RAM-backed PFN range, so add:

```c
void *ioremap(unsigned long phys, size_t size);
void iounmap(void *virt);
```

For this driver, `ioremap()` can:

- Round `phys` and `size` to page boundaries.
- Allocate kernel page-table pages if needed.
- Map the physical frames as present, read/write, kernel-only, uncached where
  the page-table format allows it.
- Return `KERNEL_BASE`-based virtual addresses.

Add `readl()`/`writel()` (and 16-bit variants if useful) that operate on
`volatile void *` MMIO addresses.

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
hda7 B 3 7
hda8 B 3 8
```

Each present disk consumes `HD_MINORS_PER_DISK` (17) minors: minor 0 is the
whole disk and minors 1-16 are partition minors.  The static device list
currently exposes partitions 1-8.  Disk `d` starts at minor
`d * HD_MINORS_PER_DISK`, giving `/dev/hda` through `/dev/hdf`.

Keep `HD_MAJOR` below `MAX_BLKDEV`; the existing limit is 64.

Update `doc/devices.txt` after the major assignment is implemented.

## AHCI Design

### HBA Register Block

The ABAR register block starts with global HBA registers followed by one
32-register block per implemented port.

| Offset | Register |
|---|---|
| 0x00 | HBA Capabilities (`CAP`) |
| 0x04 | Global HBA Control (`GHC`) |
| 0x08 | Interrupt Status (`IS`) |
| 0x0c | Ports Implemented (`PI`) |
| 0x10 | Version |
| 0x24 | HBA Capabilities Extended (`CAP2`) |

For each port `p`:

```text
Px offset = 0x100 + p * 0x80
```

| Offset | Register |
|---|---|
| 0x00 | PxCLB / PxCLBU: command list base |
| 0x08 | PxFB / PxFBU: received-FIS base |
| 0x10 | PxIS: interrupt status |
| 0x14 | PxIE: interrupt enable |
| 0x18 | PxCMD: port command/status |
| 0x20 | PxTFD: task file data |
| 0x24 | PxSIG: signature |
| 0x28 | PxSSTS: SATA status |
| 0x2c | PxSCTL: SATA control |
| 0x30 | PxSERR |
| 0x34 | PxSACT: active slots |
| 0x38 | PxCI: command issue |
| 0x3c | PxSNTF |

Important bits:

- `GHC.AE` (`1 << 31`): enable AHCI.
- `GHC.IE` (`1 << 1`): enable global interrupts.
- `PxCMD.ST` (`1 << 0`): start DMA engine.
- `PxCMD.FRE` (`1 << 4`): enable FIS receive.
- `PxCMD.FR` (`1 << 14`): FIS receive running.
- `PxCMD.CR` (`1 << 15`): command list running.
- `PxTFD.BSY` (`1 << 7`), `PxTFD.DRQ` (`1 << 3`),
  `PxTFD.ERR` (`1 << 0`).

### Controller Initialization

1. Map the ABAR and verify `GHC.AE` is supported by `CAP.SAM` if applicable.
2. Set `GHC.AE`.
3. For each bit set in `PI`, configure implemented ports that have a disk, up
   to the driver's `HD_MAX_DISKS` limit.
4. Stop the port command engine and FIS receive if they are running:
   - clear `PxCMD.ST`;
   - clear `PxCMD.FRE`;
   - poll until `PxCMD.CR` and `PxCMD.FR` clear.
5. Allocate the per-port data structures described below, zero them, and
   program `PxCLB`/`PxCLBU` and `PxFB`/`PxFBU`.
6. Enable `PxCMD.FRE`, then `PxCMD.ST`.
7. Wait for `PxSSTS.DET` to indicate a present device (`0x3`); if it remains
   absent, skip this port and continue with the next implemented port.  If no
   port has a disk, `hd_init()` returns without registering the block device.

### AHCI Memory Structures

Allocate the following with page allocator helpers that return
physically-contiguous memory, and use `__phy()` when programming device
registers:

- One command list, 1 KiB aligned, 32 bytes per slot.
- One received-FIS area, 256-byte aligned, at least 256 bytes.
- One command table per used slot, 128-byte aligned, containing:
  - a 64-byte command FIS;
  - a 16-byte ATAPI area (unused for hard disks);
  - a PRDT entry.

Command header layout:

- `CFL`: command FIS length in 32-bit words (`5` for a register FIS).
- `PRDTL`: PRDT length in entries.
- `CTBA`/`CTBAU`: physical address of the command table.
- `W`: write command when set.
- `C`: clear `PxTFD.BSY` on command-list update.

PRDT entry layout:

- `DBA`/`DBAU`: data buffer physical address.
- `DBC`: byte count minus one in bits `0:21`; bit 31 requests an interrupt on
  completion.

### AHCI Command Submission

For a single-sector request:

1. Wait for the command slot to be idle (`PxCI` bit clear).
2. Fill the command FIS:
   - FIS type `0x27` (host-to-device register FIS);
   - `C` bit set;
   - command register set to `IDENTIFY_DEVICE (0xec)`,
     `READ_SECTORS_EXT (0x24)`, or `WRITE_SECTORS_EXT (0x34)`.
3. Fill LBA and sector-count fields for **LBA48**:
   - count low/high (`1` for one sector);
   - LBA low/mid/high at FIS bytes 4-6;
   - device register with LBA bit set;
   - extended LBA low/mid/high at FIS bytes 8-10.
4. Set the PRDT to the kernel data buffer.
5. Set `PxCI` for the command slot.
6. Block on the port completion object until the interrupt handler wakes the
   waiter or a timeout expires.
7. On completion:
   - inspect `PxTFD.ERR`/`DF` and `PxIS.TFES`;
   - return `-EIO` on error;
   - return `-ETIMEDOUT` if no interrupt arrives before the timeout.

### IRQ-Driven Completion

Normal reads/writes must not poll the AHCI status registers.

- Add `request_irq(irq, handler, dev)` / `free_irq(irq, dev)`.
- Route the AHCI PCI interrupt line to a handler in `ahci.c`.
- The handler:
  - reads `IS` and `PxIS`;
  - clears the handled bits by writing them back;
  - acknowledges the PIC;
  - wakes the per-port wait queue for the completed command.
- Use `sleep_on()`/`wake_up_all()` or a `struct semaphore` as the port
  completion object. The latter is simpler because the command path has one
  waiter per port.

Boot-time identify is special because `blkdev_init()` runs before the first
user task and before `sti()`. The identify path may poll `PxIS` with a bounded
timeout while interrupts are still masked. All later read/write requests use
IRQ completion.

### LBA48 Requirements

The driver must reject devices that do not report LBA48 support in the
identify block:

- Word 83 bit 10: 48-bit addressing supported.
- Word 86 bit 10: 48-bit addressing enabled/supported.
- Words 100-103: 64-bit maximum user-addressable LBA.

Use:

- `IDENTIFY_DEVICE (0xec)` for discovery.
- `READ_SECTORS_EXT (0x24)` for reads.
- `WRITE_SECTORS_EXT (0x34)` for writes.
- `FLUSH_CACHE_EXT (0xea)` for write-back cache flush.

Validate every command against the 48-bit sector count and the partition
range before programming the FIS.

## Partition Design

### Internal Table

Add an internal fixed-size partition table in `ahci.c`:

```c
struct hd_partition {
    uint64_t start_sector;
    uint64_t nr_sectors;
    uint8_t present;
};
```

Minor 0 means the whole disk. Minors 1 and up map to parsed partitions.

`hd_get_info()` reports:

- minor 0: total sectors from identify words 100-103.
- minor > 0: sectors from the partition table, or `-ENOENT` if not present.

`hd_read()`/`hd_write()`:

- minor 0: use the LBA directly.
- minor > 0: translate to `partition.start_sector + sector`, then range-check
  against `partition.nr_sectors`.

### MBR Parsing

At init, after a successful identify:

1. Read sector 0 using the AHCI command path, not `bread()`.
2. Check the boot signature `0xaa55` at offset 510.
3. Walk the four 16-byte MBR partition entries at offset 446.
4. Record entries with a nonzero type. Use the LBA start at entry offset 8 and
   sector count at offset 12. MBR fields remain 32-bit.

### EBR Parsing

The generated `hd.img` uses an extended partition containing logical
partitions. Add a bounded EBR walk:

- The first entry in an EBR describes a logical partition; its start LBA is
  relative to the current EBR sector.
- The second entry points to the next EBR; its start LBA is relative to the
  start of the extended partition.
- Stop when the next-ebr start is zero or a safety iteration limit is reached.

This makes `/dev/hda5`, `/dev/hda6`, and `/dev/hda7` work with the existing
`gen_image.sh` layout.

## Raw Block Device File Operations

`src/fs/block/file.c` should provide real `blkdev_fops` so userspace can test
with `dd`, `cmp`, `hexdump`, and eventually mount:

- `blkdev_file_open`:
  - Call `blkdev_get_info()` and fail if the minor is absent.
- `blkdev_file_lseek`:
  - Validate against the byte size from `blkdev_get_info()`.
  - Support `SEEK_SET`, `SEEK_CUR`, and `SEEK_END`.
- `blkdev_file_read`/`blkdev_file_write`:
  - Require `f_pos` and `count` to be 512-byte aligned in the first version.
  - Convert bytes to sectors and call `blkdev_read()`/`blkdev_write()`.
  - Return `-EINVAL` for unaligned access; a later bounce-buffer version can
    remove that restriction.
- `blkdev_file_ioctl`:
  - Add `BLKGETSIZE`, `BLKGETSIZE64`, and `BLKSSZGET` to
    `src/include/fs/ioctl.h`.

This applies to all block devices, including ramdisks, so it should be
implemented independently of the AHCI driver and tested against `/dev/rd0`
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
hd_init();   /* registers HD_MAJOR only if an AHCI disk is present */
```

`hd_init()` must not call `bread()` and must never change `ROOT_DEV`.

## QEMU Configuration

Update `qemu/Makefile` so the standard disk is on an AHCI controller instead of
the default IDE controller:

```make
QEMU_FLAGS := \
	-boot order=c \
	-m 256M \
	-drive id=disk,file=hd.img,format=raw,if=none \
	-device ahci,id=ahci \
	-device ide-hd,drive=disk,bus=ahci.0 \
	-display $(QEMU_DISPLAY) \
	-no-reboot -no-shutdown
```

If the selected machine/profile requires it, use `-machine q35` with the
equivalent AHCI/IDE attachment. `quick-run` keeps its current no-disk
`-kernel`/`-initrd` flags and must remain bootable.

## Milestones

### Milestone 1: PCI and MMIO foundations

- Add PCI config-space access and class-code scanning.
- Add `ioremap()`/`iounmap()` and `readl()`/`writel()`.
- Add a debugfs or boot log that reports the discovered AHCI BAR and interrupt
  line.

### Milestone 2: AHCI identify and whole-disk registration

- Initialize the AHCI controller and all implemented ports with attached disks.
- Allocate command structures and submit `IDENTIFY_DEVICE`.
- Require LBA48 support and read words 100-103.
- Register major `HD_MAJOR` once and expose whole-disk minors for each disk.
- Add `/dev/hda` through `/dev/hdf` to `rd/device_list`.

### Milestone 3: IRQ-driven LBA48 read/write

- Add IRQ registration and route the AHCI interrupt to the driver.
- Implement `READ_SECTORS_EXT` and `WRITE_SECTORS_EXT`.
- Validate all LBA/count ranges before command submission.
- Implement raw block fops and test `/dev/rd0` first.

### Milestone 4: MBR and EBR partition nodes

- Parse MBR sector 0 and EBR chains.
- Build minors 1-4 and logical minors 5+.
- Add the device nodes to `rd/device_list` and `doc/devices.txt`.
- Range-check reads/writes against partition sizes.

### Milestone 5: Write safety and flush

- Add `FLUSH_CACHE_EXT`.
- Use a snapshot or disposable disk for write tests.
- Confirm `quick-run` still boots without an AHCI controller.

### Milestone 6: Userspace integration

- Test raw sector reads against known values in `hd.img`.
- Format or write a MarioFS image to a test partition and mount it with
  `mount -t mariofs /dev/hdaN /mnt`.
- Add a shell test or small `app/test/hd` program.

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

Read-only tests use the normal boot disk. Write tests use
`qemu-system-i386 -snapshot` or a separate disposable disk image so the checked
in `qemu/hd.img` is not modified.

Automated test ideas:

- Add `tests/shell/hd.test` using `dd`/`hexdump` to read the MBR signature.
- Add a small `app/test/hd` program that opens `/dev/hda`, checks geometry,
  reads sector 0, and prints a stable checksum.
- Keep disk write coverage in a separate QEMU invocation or `-snapshot` mode.

## Risks

- PCI enumeration must tolerate systems with no AHCI controller and return
  quickly.
- MMIO mapping must not assume the AHCI BAR is in the already-mapped RAM range.
- IRQ dispatch must avoid regressing the existing hard-coded PIT, PS/2, and
  UART handlers.
- AHCI reset sequences must use bounded waits; a floating or unresponsive
  controller must not hang boot.
- Command-table and received-FIS allocations must be physically contiguous
  and correctly aligned.
- 48-bit sector values must not be truncated when passing through the existing
  `unsigned long` block-device callbacks on 32-bit systems; sector numbers fit
  in 32 bits for the current image, but partition accounting should use
  `uint64_t`.
- Raw block fops affect ramdisks too. Test `/dev/rd0` before `/dev/hda` to
  isolate regressions.

## Deferred

- Port multipliers and more than `HD_MAX_DISKS` AHCI disks.
- NCQ command submission and multiple in-flight commands.
- ATAPI/PACKET for CD-ROM devices.
- Byte-granular raw block reads/writes via a bounce buffer.
- Dynamic device nodes and `/proc/partitions`.
- Booting directly from the hard disk instead of ramdisk root.
