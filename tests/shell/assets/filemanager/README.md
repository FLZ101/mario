# File Manager test volumes

`compact.ext2.gz` and `compact.fat32.gz` contain empty 1 MiB filesystem images.
The copy test writes them to scratch partitions in its QEMU snapshot, then
fills each until a copy can write some data but cannot complete. This avoids
filling the standard 64 MiB partitions in the guest.

Run `bash tests/shell/assets/filemanager/generate.sh` on a host with `mke2fs`,
`mkfs.fat`, and `gzip` to regenerate them. `mkfs.fat` warns that the FAT32
cluster count is below the usual minimum; Mario's VFAT parser accepts this
compact test volume.
