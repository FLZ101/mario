# GPT Partition Support Plan

## Goal

Add GUID Partition Table (GPT) support to the disk stack so Mario can:

- Recognize GPT disks in the kernel block-device layer.
- Create `/dev/hd*` partition nodes from GPT entries.
- Fall back to the existing MBR/EBR parser when GPT is absent or invalid.

The current AHCI driver already submits LBA48 read/write commands, but the
public block-device callbacks still take 32-bit `unsigned long` sector
arguments. GPT parsing must understand 64-bit on-disk LBA fields and validate
them against the current block-device API limits.

## Scope

This plan covers:

1. Kernel-side GPT header and partition-entry parsing.
2. Integration with the hard-disk partition table and `/dev/hd*` device nodes.
3. Protective MBR and backup GPT fallback.

GPT does not replace the current MBR path. A disk may use either GPT or MBR;
the parser must detect which layout is present.

## Current State

The AHCI driver in `src/fs/block/ahci.c` already provides:

- LBA48 `IDENTIFY_DEVICE`, `READ_SECTORS_EXT`, and `WRITE_SECTORS_EXT`.
- IRQ-driven command completion and boot-time polling helpers.
- MBR/EBR partition parsing into `struct hd_disk.partitions`.
- Static `/dev/hda` through `/dev/hdf` and `/dev/hd*1` through `/dev/hd*8`
  nodes in `rd/device_list`.
- `HD_MAX_PARTITIONS` of 16 and per-disk `HD_MINORS_PER_DISK` of 17.

There is no GPT header, CRC32, GUID, or GPT partition-entry parser in the
kernel.

## GPT On-Disk Layout

### Protective MBR

LBA 0 must contain:

- A standard MBR signature `0x55aa` at offset 510.
- At least one partition entry with type `0xee`.
- The `0xee` entry usually covers the whole disk or as much as the MBR can
  represent.

The protective MBR is only a compatibility structure. If a valid GPT header is
present, GPT wins; legacy partition entries in the protective MBR are ignored.

### GPT Header

The primary header is normally at LBA 1. Important fields:

| Offset | Field |
|---|---|
| 0 | signature, must be `"EFI PART"` |
| 8 | revision |
| 12 | header size |
| 16 | header CRC32 |
| 24 | current LBA |
| 32 | backup LBA |
| 40 | first usable LBA |
| 48 | last usable LBA |
| 56 | disk GUID |
| 72 | partition entry array LBA |
| 80 | number of partition entries |
| 84 | size of one partition entry |
| 88 | partition entry array CRC32 |

Validate:

- Signature is `"EFI PART"`.
- `header_size >= 92` and not larger than the sector size.
- Header CRC matches with the CRC field zeroed.
- `current_lba == 1` for the primary header.
- `backup_lba > current_lba`.
- `first_usable_lba <= last_usable_lba`.
- Partition entry array LBA and size are within the disk.
- `num_partition_entries` is bounded, for example no more than 256.
- `entry_size >= 128`; the standard value is 128.

For the backup header:

- Read it from `disk_sectors - 1`.
- `current_lba` is the backup LBA, not 1.
- If valid, use its partition-entry array LBA, count, size, and CRC.

### Partition Entry Array

Each standard entry is 128 bytes:

| Offset | Field |
|---|---|
| 0 | partition type GUID, 16 bytes |
| 16 | unique partition GUID, 16 bytes |
| 32 | first LBA, 64-bit little-endian |
| 40 | last LBA, 64-bit little-endian |
| 48 | attributes, 64-bit |
| 56 | UTF-16LE partition name, 72 bytes |

Parsing rules:

- Skip entries with `first_lba == 0 && last_lba == 0`.
- Reject entries whose range is outside `first_usable_lba..last_usable_lba`.
- Reject entries where `first_lba > last_lba`.
- Keep the entry index plus one as the initial minor-number mapping:
  - GPT entry 0 -> `/dev/hda1`
  - GPT entry 1 -> `/dev/hda2`
  - unused entries are skipped, but their minor remains unused.

## CRC32 and GUID Helpers

Add:

- `src/include/lib/crc32.h`
- `src/lib/crc32.c`

Use the standard reflected CRC32 polynomial `0xedb88320`.

Add small GUID helpers:

```c
int guid_equal(const uint8_t *a, const uint8_t *b);
int guid_is_zero(const uint8_t *a);
```

GPT structures use little-endian fields. Mario currently runs only on i386, but
the parser should still use fixed-width `__u16`/`__u32`/`__u64` types and
packed structures.

## Kernel GPT Parser

### New Files

- `src/include/fs/gpt.h`
- `src/fs/block/gpt.c`
- `src/include/lib/crc32.h`
- `src/lib/crc32.c`

`gpt.c` exports a parser that does not call `bread()` or `blkdev_read()`.
GPT detection runs from `hd_init()` before the buffer cache can safely be used
for disk probing, so `ahci.c` supplies a raw AHCI sector reader:

```c
struct gpt_partition {
    uint64_t first_lba;
    uint64_t last_lba;
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t attrs;
    uint8_t present;
};

typedef int (*gpt_read_sectors_fn)(void *priv, uint64_t lba,
                                   unsigned int nr, void *buf);

int gpt_parse(void *priv, uint64_t disk_sectors,
              gpt_read_sectors_fn read_sectors,
              struct gpt_partition *parts, unsigned int max_parts,
              unsigned int *nr_parts);
```

`ahci.c` owns the `struct hd_disk` and passes a thin wrapper around
`ahci_read_sectors_poll()` plus that disk pointer as `priv`. The normal
IRQ-driven `ahci_read_sectors()` path is not used for GPT probing because the
parser runs from `hd_init()` while interrupts are still masked.

### Parsing Steps

1. Call `read_sectors(priv, 0, 1, buf)` and verify the `0x55aa` signature.
2. Call `read_sectors(priv, 1, 1, buf)` and try to validate the primary GPT
   header.
3. If the primary header is invalid, call
   `read_sectors(priv, disk_sectors - 1, 1, buf)` and try the
   backup header.
4. Validate the partition-entry array CRC:
   - Allocate a temporary buffer of `num_entries * entry_size` bytes.
   - Read the array from `partition_entry_lba` through `read_sectors()`.
   - Compute the CRC over exactly `num_entries * entry_size` bytes.
5. If both primary and backup GPTs are invalid, return a "not GPT" result so
   `ahci.c` falls back to MBR/EBR parsing.
6. Fill `parts[]` from valid non-empty entries and set `nr_parts`.

The temporary entry buffer can be allocated with `kmalloc()` during
`hd_init()`; the page allocator is already initialized at that point. GPT
probing must use the existing single-sector polling helper in `ahci.c`, even
if the normal read path is IRQ-driven.

### LBA Width Compatibility

GPT stores 64-bit LBAs. The current block-device API uses:

```c
int (*blkdev_read)(dev_t dev, unsigned long sector, unsigned long nr, ...);
```

On i386 `unsigned long` is 32 bits. For now:

- Parse and store GPT LBAs as `uint64_t`.
- Reject or mark unusable any partition whose `last_lba` does not fit in the
  32-bit public `sector` argument.
- Respect `hd_get_info()`'s current `int nr` limit; partitions larger than
  `0x7fffffff` sectors cannot be represented yet and must be skipped.
- Keep AHCI LBA48 as the physical command limit, even though the public API
  remains 32-bit.

A later milestone can widen `blkdev_read()`/`blkdev_write()` and
`struct blkdev_info.nr` to 64 bits.

## Integration with `ahci.c`

`struct hd_partition` in `src/fs/block/ahci.c` already stores 64-bit
`start_sector` and `nr_sectors` fields, so GPT support does not need to widen
the partition table. GPT parsing fills the same `disk->partitions[]` array as
the MBR/EBR parser.

Detection order in `hd_init()`:

1. AHCI port initialization and `IDENTIFY_DEVICE`.
2. Validate LBA48 support and read the 64-bit sector count.
3. Try `gpt_parse(disk, disk->total_sectors, ahci_gpt_read, parts, ...)`.
4. If GPT succeeds, populate `disk->partitions[]` from GPT entries.
5. If GPT is not present or invalid, call the existing `hd_parse_mbr()` path.

`hd_read()`/`hd_write()` translation remains unchanged:

```c
if (minor == 0)
    use whole disk;
else if (minor partition present)
    check sector range and add partition.start_sector;
else
    return -ENOENT;
```

`hd_get_info()` reports GPT partition sizes for present minors and returns
`-ENOENT` for absent minors.

## Device Nodes

GPT allows up to 128 standard partitions, but the current AHCI driver caps each
disk at `HD_MAX_PARTITIONS` (16). `rd/tools/rd-make.c` already has
`MAX_DEV` set to 128, so no ramdisk-maker limit change is required.

Plan:

- Keep the current `HD_MINORS_PER_DISK` value of 17.
- Add the missing static partition nodes `hd{a..f}9` through `hd{a..f}16`.
- Keep the existing `hda` through `hdf` whole-disk nodes and `hd*1` through
  `hd*8` partition nodes.
- Map GPT entries in ascending entry order to `hda1`, `hda2`, etc.
- Do not create a node for unused entries; accessing that minor returns
  `-ENOENT`.
- Document the limit in `doc/devices.txt`.

This keeps the static ramdisk device model while supporting typical GPT test
images. Dynamic device nodes and `/proc/partitions` remain deferred.

## Files to Change

### New

- `src/include/fs/gpt.h`
- `src/fs/block/gpt.c`
- `src/include/lib/crc32.h`
- `src/lib/crc32.c`
- Optionally `src/include/lib/guid.h` / `src/lib/guid.c`

### Modified

- `src/fs/block/ahci.c`
  - Add `hd_parse_gpt()` and call it before `hd_parse_mbr()`.
  - Expose or add a private `ahci_gpt_read()` wrapper around the boot-time
    AHCI polling read path.
- `src/include/fs/ahci.h`
  - Share GPT partition limits and constants if GPT parsing is kept separate
    from the AHCI driver.
- `rd/device_list`
  - Add `hd{a..f}9` through `hd{a..f}16`.
- `doc/devices.txt`
  - Document GPT node naming and limits.

No `src/Makefile` change is needed for new C files.

## Milestones

### Milestone 1: Kernel GPT parser

- Add CRC32 and GUID helpers.
- Add GPT on-disk structures.
- Implement `gpt_parse()` with primary header validation.
- Add `hd_parse_gpt()` to `ahci.c` and call it before `hd_parse_mbr()`.
- Test with a small GPT disk image attached to QEMU as an AHCI disk.

### Milestone 2: Backup GPT and MBR fallback

- Implement backup-header fallback from the last disk LBA.
- Preserve MBR/EBR parsing when GPT is invalid.
- Add partition range validation.

### Milestone 3: Device nodes and userspace visibility

- Add `hd{a..f}9` through `hd{a..f}16` static nodes.
- Verify missing minors return `-ENOENT`.
- Read GPT partitions with `dd`/`hexdump`.

### Deferred

- Full 64-bit `blkdev_read()`/`blkdev_write()` and `blkdev_info.nr`.
- More than 16 partition minors per disk.
- GPT hybrid MBR handling.
- `/proc/partitions` and dynamic device nodes.
- GUID-specific partition policy, such as preferring only ESP/basic-data
  partitions for boot discovery.
- GPT disk labels, partition names, and UUIDs exposed to userspace.

## Validation

Create a disposable GPT disk on the host:

```sh
truncate -s 256M gpt.img
sgdisk --clear --new=1:2048:100000 --typecode=1:ef00 gpt.img
mkfs.fat -F 32 --offset 2048 gpt.img
```

Or use a loop device with partitions:

```sh
truncate -s 256M gpt.img
sgdisk --clear --new=1:2048:100000 --typecode=1:8300 gpt.img
sudo losetup -P loop0 gpt.img
sudo mkfs.ext2 /dev/loop0p1
sudo losetup -d /dev/loop0
```

Expose `gpt.img` to QEMU as an AHCI disk using the same attachment style as
`qemu/Makefile`:

```sh
qemu-system-i386 \
  -m 256M \
  -drive id=gpt,file=gpt.img,format=raw,if=none \
  -device ahci,id=ahci \
  -device ide-hd,drive=gpt,bus=ahci.0 \
  -kernel tmp/kernel.exe -initrd tmp/rdx \
  -display curses -no-reboot -no-shutdown
```

In the shell:

```text
ls -l /dev/hda /dev/hda1
dd if=/dev/hda of=/root/gpt_header.bin bs=512 skip=1 count=1
hexdump -C -n 64 /root/gpt_header.bin
dd if=/dev/hda1 of=/root/gpt_part.bin bs=512 count=1
```

Expected:

- `dd` from LBA 1 shows `EFI PART`.
- `/dev/hda1` has the correct partition size.
- Reading beyond the partition returns an error.
- A legacy MBR disk still exposes MBR partitions.
- A damaged primary GPT header still allows the kernel to use the backup GPT.

## Risks

- The GPT header is at LBA 1, not at sector 0; probing sector 0 first is a
  common mistake.
- GPT CRC validation is mandatory. Reading a corrupt primary GPT and
  interpreting stale entries can expose bogus partitions.
- The backup GPT may have different `current_lba` and entry-array LBA values;
  do not hard-code those fields.
- The public block API still uses 32-bit `sector` arguments and an `int` sector
  count in `blkdev_get_info()`. GPT parsing must reject partitions that cannot
  be represented by those interfaces even though AHCI commands use LBA48.
- Device nodes are static. Adding `hd{a..f}9` through `hd{a..f}16` must stay
  within `rd/tools/rd-make.c`'s current `MAX_DEV` limit of 128.
