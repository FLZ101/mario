#!/usr/bin/env bash
set -euo pipefail

output=$(cd -- "$(dirname -- "$0")" && pwd)
work_dir=$(mktemp -d)
trap 'rm -rf -- "$work_dir"' EXIT

truncate -s 1M "$work_dir/compact.ext2"
E2FSPROGS_FAKE_TIME=1700000000 mke2fs -q -t ext2 -b 1024 -N 128 -m 0 \
    -U 525742d6-4033-4eb5-bb76-54a2415ec2c9 -E hash_seed=37719e33-8b82-4b46-86c7-c84f81703387 \
    "$work_dir/compact.ext2"
mkfs.fat --invariant -F 32 -S 512 -s 1 -C "$work_dir/compact.fat32" 1024
gzip -n -c "$work_dir/compact.ext2" >"$output/compact.ext2.gz"
gzip -n -c "$work_dir/compact.fat32" >"$output/compact.fat32.gz"
