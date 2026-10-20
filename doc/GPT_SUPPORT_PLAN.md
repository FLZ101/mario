# GPT Partition Support Plan

## Goal

Add GUID Partition Table (GPT) support to the disk stack so Mario can:

- Recognize GPT disks in the kernel block-device layer.
- Create `/dev/hd*` partition nodes from GPT entries.
- Fall back to the existing MBR/EBR parser when GPT is absent or invalid.
- Optionally boot from a GPT disk through the custom `boot.bin` FAT32 path.

The current ATA target in `doc/HARDDISK_DRIVER_PLAN.md` uses LBA28 and small
QEMU disks. GPT parsing should understand 64-bit on-disk LBA fields, but the
initial kernel block API can remain 32-bit as long as disks fit the existing
LBA28/32-bit limits.

## Scope

This plan covers:

1. Kernel-side GPT header and partition-entry parsing.
2. Integration with the hard-disk partition table and `/dev/hd*` device nodes.
3. Protective MBR and backup GPT fallback.
4. `boot.bin` GPT discovery for full-disk boot.

GPT does not replace the current MBR path. A disk may use either GPT or MBR;
the parser must detect which layout is present.

## Current State

The hard-disk plan proposes:

- `src/fs/block/hd.c` for ATA PIO I/O and MBR/EBR partition parsing.
- Static `/dev/hda`, `/dev/hda1`, etc. nodes in `rd/device_list`.
- A partition table mapping minor numbers to LBA ranges.

The custom boot loader in `src/boot/boot.asm` currently:

- Reads LBA 0 as an MBR.
- Scans the four MBR partition entries at offset 446.
- Follows extended partitions through EBRs.
- Searches FAT32 partitions for `BOOT.BIN`.

There is no GPT header, CRC32, GUID, or 64-bit LBA helper code in the kernel.

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

`gpt.c` exports:

```c
struct gpt_partition {
    uint64_t first_lba;
    uint64_t last_lba;
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint16_t attrs;
};

int gpt_parse(dev_t disk, uint64_t disk_sectors,
              struct gpt_partition *parts, unsigned int max_parts,
              unsigned int *nr_parts);
```

`hd.c` owns the actual `dev_t` and performs reads through the raw ATA PIO path,
not through `bread()`, because `hd_init()` runs before the buffer cache.

### Parsing Steps

1. Read LBA 0 and verify the `0x55aa` signature.
2. Read LBA 1 and try to validate the primary GPT header.
3. If the primary header is invalid, read the disk's last LBA and try the
   backup header.
4. Validate the partition-entry array CRC:
   - Allocate a temporary buffer of `num_entries * entry_size` bytes.
   - Read the array from `partition_entry_lba`.
   - Compute the CRC over exactly `num_entries * entry_size` bytes.
5. If both primary and backup GPTs are invalid, return a "not GPT" result so
   `hd.c` falls back to MBR/EBR parsing.
6. Fill `parts[]` from valid non-empty entries and set `nr_parts`.

The temporary entry buffer can be allocated with `kmalloc()` during
`hd_init()`; the page allocator is already initialized at that point.

### LBA Width Compatibility

GPT stores 64-bit LBAs. The current block-device API uses:

```c
int (*blkdev_read)(dev_t dev, unsigned long sector, unsigned long nr, ...);
```

On i386 `unsigned long` is 32 bits. For now:

- Parse and store GPT LBAs as `uint64_t`.
- Reject or mark unusable any partition whose `last_lba` does not fit in 32
  bits.
- Keep ATA LBA28 as the physical limit.

A later milestone can widen the block-device API to `uint64_t` when LBA48/AHCI
are implemented.

## Integration with `hd.c`

Modify the hard-disk partition table to hold 64-bit GPT ranges:

```c
struct hd_partition {
    uint64_t start_lba;
    uint64_t nr_sectors;
    uint8_t present;
};
```

Detection order in `hd_init()`:

1. ATA identify.
2. Read sector 0.
3. Try `gpt_parse()`.
4. If GPT succeeds, populate minors from GPT entries.
5. If GPT is not present or invalid, use the existing MBR/EBR path.

`hd_read()`/`hd_write()` translation remains the same:

```c
if (minor == 0)
    use whole disk;
else if (minor partition present)
    check sector range and add partition.start_lba;
else
    return -ENOENT;
```

`hd_get_info()` reports GPT partition sizes for present minors and returns
`-ENOENT` for absent minors.

## Device Nodes

GPT allows up to 128 standard partitions. The current ramdisk device table has
a `MAX_DEV` limit of 32 in `rd/tools/rd-make.c`.

Plan:

- Increase `MAX_DEV` to at least 64.
- Expose `/dev/hda` plus a fixed set of partition nodes, for example
  `hda1` through `hda16`.
- Map GPT entries in ascending entry order to `hda1`, `hda2`, etc.
- Do not create a node for unused entries; accessing that minor returns
  `-ENOENT`.
- Document the limit in `doc/devices.txt`.

This keeps the static ramdisk device model while supporting typical GPT test
images. Dynamic device nodes and `/proc/partitions` remain deferred.

## Bootloader GPT Discovery

The kernel GPT parser alone is sufficient when Mario is booted through
GRUB/GRUB4DOS or QEMU `-kernel` and the root filesystem remains ramdisk.

To support the custom `boot.bin` path on a GPT disk, `src/boot/boot.asm`
stage 1 must learn to discover the FAT32 partition without relying on MBR
partition type bytes `0x0b`/`0x0c`.

### Real-Mode Algorithm

In stage 1:

1. Read LBA 0 and check for a protective MBR with a `0xee` entry.
2. Read LBA 1 and check the GPT header signature `"EFI PART"`.
3. Read GPT fields needed in real mode:
   - partition entry array LBA (64-bit),
   - number of partition entries,
   - entry size.
4. For each 512-byte sector of the entry array:
   - Read it into the existing temporary buffer.
   - Scan entries whose 128-byte slots lie in that sector.
   - Skip empty entries.
   - Read the partition's first LBA and treat it as a possible FAT32 boot
     partition.
   - Read that sector and check the FAT32 BPB.
   - If FAT32, run the existing `_fat32`/`find_it` path to search for
     `BOOT.BIN`.
5. Stop when `BOOT.BIN` is found.

The existing `DAP_lba` field is already 64 bits. The first version can require
the partition-start LBA to fit in 32 bits and zero the high dword. Full 64-bit
real-mode LBA arithmetic is deferred.

### Assembly Constraints

The bootloader has a very small real-mode memory budget. Do not try to load the
full 16 KiB GPT entry array at once. Read and scan it one 512-byte sector at a
time.

The GPT parser should be a separate stage-1 routine, leaving the existing
MBR/EBR routine intact. A GPT-aware disk takes the new path; a legacy MBR disk
takes the old path.

## Files to Change

### New

- `src/include/fs/gpt.h`
- `src/fs/block/gpt.c`
- `src/include/lib/crc32.h`
- `src/lib/crc32.c`
- Optionally `src/include/lib/guid.h` / `src/lib/guid.c`

### Modified

- `src/fs/block/hd.c`
  - Add GPT parsing and 64-bit partition fields.
- `src/include/fs/hd.h` if created by the hard-disk plan
  - Share GPT partition limits and constants.
- `rd/device_list`
  - Add fixed GPT partition nodes.
- `rd/tools/rd-make.c`
  - Increase `MAX_DEV`.
- `doc/devices.txt`
  - Document GPT node naming and limits.
- `src/boot/boot.asm`
  - Add stage-1 GPT discovery.

No `src/Makefile` change is needed for new C files.

## Milestones

### Milestone 1: Kernel GPT parser

- Add CRC32 and GUID helpers.
- Add GPT on-disk structures.
- Implement `gpt_parse()` with primary header validation.
- Wire GPT detection into `hd_init()`.
- Test with a small GPT disk image mounted through QEMU.

### Milestone 2: Backup GPT and MBR fallback

- Implement backup-header fallback from the last disk LBA.
- Preserve MBR/EBR parsing when GPT is invalid.
- Add partition range validation.

### Milestone 3: Device nodes and userspace visibility

- Increase `MAX_DEV`.
- Add `hda1` through `hdaN` static nodes.
- Verify missing minors return `-ENOENT`.
- Read GPT partitions with `dd`/`hexdump`.

### Milestone 4: GPT boot path

- Add stage-1 GPT discovery to `boot.bin`.
- Boot from a GPT disk containing a FAT32 `BOOT.BIN` partition.
- Preserve MBR boot behavior.

### Deferred

- Full 64-bit block-device API and LBA48/AHCI support.
- More than 16 static partition nodes.
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

Expose `gpt.img` to QEMU as an IDE disk, boot with `-kernel`/`-initrd`, and:

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

For the bootloader path, install `boot.bin` and `kernel.exe` in a FAT32 GPT
partition and verify `make -C qemu run` reaches Mario. Keep the existing MBR
`hd.img` path working.

## Risks

- The GPT header is at LBA 1, not at sector 0; probing sector 0 first is a
  common mistake.
- GPT CRC validation is mandatory. Reading a corrupt primary GPT and
  interpreting stale entries can expose bogus partitions.
- The backup GPT may have different `current_lba` and entry-array LBA values;
  do not hard-code those fields.
- The block API currently truncates 64-bit LBAs. Parse and validate them, but
  reject partitions that do not fit the current implementation.
- Real-mode bootloader GPT support is memory- and register-constrained; reading
  the entry array sector-by-sector is safer than loading the full array.
- Device nodes are static. Adding too many `hdaN` nodes can exceed the
  ramdisk-maker limit unless `MAX_DEV` is raised.
