# MarioFS Seek Performance Plan

## Goal

Replace MarioFS's single-position block cache and contiguous-prefix optimization
with a lazily built full block map per regular-file inode. Random, backward, and
forward seeks should then require only one chain walk per inode, after which
block-number lookup is O(1).

The measurable success criterion is a reduction in the execution time of
`tests/shell/mariofs.perf.test`.

## Benchmark

`tests/shell/mariofs.perf.test` is the performance benchmark for this change.
It runs the same Python heredoc script three times. Each run imports every
module listed by `sys.builtin_module_names` and then keeps the existing
`lseek()`/`read()` test on a regular MarioFS file. The builtin-module sweep
exercises different parts of the Python executable, while the file read test
keeps the random-seek block-chain workload.

Run only the benchmark with:

```sh
.venv/bin/python tests/lit_shell_test.py --test mariofs.perf.test tests/shell
```

The shell lit runner prints the total test-case elapsed time:

```text
PASS: tests/shell/mariofs.perf.test [63.14s]
```

The script itself also runs under the shell `time` builtin, so the captured
output contains `real`, `user`, and `system` times for each of the three Python
invocations.

Use the test-case elapsed time as the primary comparison point. The expected
result after the full block map is implemented is a lower elapsed time for the
same test on the same host/build, especially the repeated random seek portion.

## Current Problem

`lseek()` itself is already O(1) through the default path in
`src/fs/read_write.c`. The expensive part happens when the next
`read()`/`write()` needs to map the new file position to a block number.

Regular files call `mario_file_read()` in `src/fs/mariofs/file.c`, which uses
`mario_nth_block()` in `src/fs/mariofs/super_block.c`. MarioFS stores each file
as a singly linked block chain:

- The first block number is stored in `inode->i_rdev`.
- The last four bytes of each 512-byte block contain the next block number.

The current `struct mario_inode_info` cache in
`src/include/fs/mariofs/mariofs.h` stores one previous block position and a
contiguous-prefix count. This works well for sequential and contiguous files,
but it still walks from block 1 for many backward and random seek patterns.
Repeated random access on a large file can therefore become O(n^2).

## Design

### Block Map Data

Remove these fields from `struct mario_inode_info`:

```c
unsigned long block_index;
unsigned long block_nr;
unsigned long contiguous_count;
```

Remove the corresponding accessors and invalidation macro:

```c
MARIO_INODE_BLOCK_INDEX
MARIO_INODE_BLOCK_NR
MARIO_INODE_CONTIGUOUS_COUNT
MARIO_INODE_INVALIDATE_BLOCK_CACHE
```

Add a full block map instead:

```c
unsigned long *block_map;
unsigned long block_map_len;
unsigned long block_map_order; /* allocation metadata */
```

`block_map_len` records the number of valid entries, and the allocation metadata
must distinguish `kmalloc()`-backed maps from `pages_alloc()`-backed maps so
they can be freed correctly.

### Map Construction

Add a helper in `src/fs/mariofs/super_block.c`:

```c
static int mario_build_block_map(struct inode *inode);
```

Construction walks the chain from `inode->i_rdev` for `inode->i_nr_block`
entries and stores every block number. Use `kmalloc()` for small maps and
`pages_alloc()`/`pages_free()` for maps too large for `kmalloc()`.

If the chain ends before `i_nr_block` entries, record only the entries actually
found. `mario_nth_block()` can still return the short chain length so existing
callers can detect corruption.

### Lookup Semantics

Rewrite `mario_nth_block()` so regular files use the map:

- If `block_map` is not built yet, build it.
- `n > 0` returns `block_map[n - 1]`.
- `n == 0` returns `block_map[block_map_len - 1]`.
- Return `block_map_len`, preserving the existing caller convention.

Directories and non-regular files do not get a block map. Keep a simple chain
walk for those cases, but remove the contiguous-prefix logic.

### Lifecycle

Add a `clear_inode` callback to `struct super_operations` in
`src/include/fs/fs.h`, call it from `clear_inode()` in `src/fs/inode.c`, and
register a `mario_clear_inode` implementation in `mario_sops`.

`mario_clear_inode()` must release `block_map` before the inode object is
reused. Without this step, an evicted MarioFS inode would leak its map.

### Invalidation

Invalidate the map only when the block chain can change:

- `mario_read_inode()` destroys any previous map before reloading the inode.
- `mario_file_truncate()` destroys the map at entry.
- `mario_file_write()` destroys the map only when it appends a new block or
  allocates the first block. An in-place overwrite keeps the map valid.
- Directory mutation functions do not need to invalidate regular-file maps.

## Files to Change

- `src/include/fs/mariofs/mariofs.h`
- `src/include/fs/fs.h`
- `src/fs/inode.c`
- `src/fs/mariofs/super_block.c`
- `src/fs/mariofs/file.c`
- `src/fs/mariofs/dir.c` if directory chain lookup is refactored

## Implementation Steps

1. Replace the cache fields and macros in `mario_inode_info`.
2. Add map allocation, construction, teardown, and invalidation helpers.
3. Add the `clear_inode` super-operation hook and call it before inode reuse.
4. Rewrite `mario_nth_block()` to use the map for regular files.
5. Update `mario_file_read()` so the first block lookup uses the map; the
   within-read next-pointer loop can remain.
6. Update `mario_file_write()` to use `mario_nth_block()` for the starting
   block instead of manually walking from block 1.
7. Update `mario_file_truncate()` to use the map for the nth block and tail
   block.
8. Remove all call sites of the old cache invalidation macro.

## Validation

- Before changing the kernel, record the baseline:

  ```sh
  .venv/bin/python tests/lit_shell_test.py --test mariofs.perf.test tests/shell
  ```

- Rebuild the kernel and ramdisk.
- Run the same `mariofs.perf.test` again and compare the printed elapsed time
  against the baseline; it should be lower after the block map is in place.
- Run the shell suite, especially:
  - `tests/shell/toybox.test`
  - `tests/shell/sbase.test`
  - mount/df tests
- Run the VGA tests to catch executable-loading regressions.
- Add or run a random-seek correctness test that writes a known pattern, seeks
  backward and forward, and compares the data.
- Add GDB or `printk` instrumentation on `mario_nth_block()` to verify that the
  chain is walked once per inode and later lookups hit the map directly.

## Deferred

A persistent on-disk extent/index format would avoid the first full chain walk,
but it requires coordinated changes in `rd/tools/mariofs.h`,
`rd/tools/rd-make.c`, and the kernel reader. This plan intentionally keeps the
on-disk format unchanged.
