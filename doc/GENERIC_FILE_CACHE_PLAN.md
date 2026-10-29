# Generic File Page Cache Plan

## Objective

Make ordinary regular-file `read()` and file-backed `mmap()` share one generic
page cache.  A page loaded by either path must satisfy a later access through
the other path without another filesystem or AHCI read.

The complete scope is ext2, MarioFS, and FAT.  Ext2 is implemented and
profiled first because `tests/shell/python-0.test` runs from ext2 and is the
performance target.

Keep both existing AHCI changes unchanged:

- multi-sector AHCI transfers; and
- `/sys/kernel/debug/ahci` command/latency profiling.

This project is a synchronous demand-cache only.  It does not add readahead,
AHCI queueing, asynchronous I/O, an ext2 indirect-block cache, or a dirty-page
writeback daemon.

It also does not add reverse mapping to revoke PTEs already installed in a
file mapping after truncate.  The kernel's current truncate behavior leaves
such PTEs present but suppresses their later writeback once the corresponding
filemap entry has been invalidated.  Preserve that behavior in this project;
do not silently claim Linux-style post-truncate fault semantics.

## Why This May Help

The traced `python-0` baseline is:

```text
test wall time: 13.06 s
AHCI reads:     6,266 commands, 50,128 sectors, 4.023 s wait
Python user:    6.210 s
```

Ext2 ordinary `read()` currently goes directly to ext2 block I/O, while mmap
faults use the existing `filemap` cache.  The same file page can therefore be
loaded twice.  A unified cache removes those duplicate data-page loads.

It cannot remove first-touch data-page reads, ext2 indirect-pointer reads, or
Python user-mode CPU time.  Therefore it is an architectural correctness and
reuse improvement, not a promise to reduce `python-0` to seven seconds by
itself.

## Public Interfaces

Extend `struct file_operations` with one uncached backing-read operation:

```c
int (*read_page)(struct inode *inode, off64_t offset,
                 char *page, unsigned long valid_bytes);
```

Contract:

- `offset` is page aligned;
- `valid_bytes` is in `[0, PAGE_SIZE]`;
- the supplied page is already zeroed;
- success returns exactly `valid_bytes`; failure returns a negative error;
- the operation does not change `file->f_pos`, access userspace, call public
  `.read`, or insert cache entries.

Add generic file-cache interfaces:

```c
enum filemap_access_kind {
    FILEMAP_ACCESS_READ,
    FILEMAP_ACCESS_MMAP,
};

int filemap_get_page(struct file *file, off64_t offset,
                     enum filemap_access_kind access_kind,
                     unsigned long *page);
int generic_file_read(struct inode *inode, struct file *file,
                      char *buf, int count);
void filemap_update_cached_range(struct inode *inode, off64_t offset,
                                 const char *kernel_bytes,
                                 unsigned long count);
```

`filemap_get_page()` returns a caller-owned page reference.  Its `offset` is
page aligned.  `generic_file_read()` passes `FILEMAP_ACCESS_READ` and
`filemap_nopage()` passes `FILEMAP_ACCESS_MMAP`; the argument selects
statistics and entry fill source only, not cache-sharing behavior.  The helper
returns `-ENOSPC` when the cache budget has no reclaimable entry and `-ENOMEM`
for real page or entry allocation failure.

## Cache Model

Cache entries are keyed by `(inode pointer, file page index)` and contain:

- page address;
- entry reference count;
- state: `LOADING`, `UPTODATE`, `INVALIDATED`, or `ERROR`;
- transient `WRITEBACK` flag on an otherwise up-to-date entry;
- saved error value for an `ERROR` entry;
- immutable fill source for an `UPTODATE` entry: normal read, mmap fault, or
  beyond-EOF mmap zero page;
- wait queue for a `LOADING` entry; and
- LRU links.

The cache entry owns one page reference.  Each `filemap_get_page()` caller
gets another temporary reference; an installed PTE owns its own reference.

### Page Acquisition

1. Look up the entry under the filemap global lock.
2. On an `UPTODATE` hit, retain the page and move the entry on the LRU.
3. On a `LOADING` hit, retain the entry, drop the global lock, and wait for
   its final state rather than issuing duplicate I/O.
4. On a miss, reserve a cache-budget slot and publish a `LOADING` entry before
   any I/O.  The reserved slot counts immediately.
5. Drop the global lock, allocate a zeroed page, serialize the backing load
   with `inode->i_sem`, and call `read_page`.
6. Reacquire the global lock and either publish `UPTODATE`, or retire the
   entry on error or invalidation.  Wake all waiters in either case.

An `INVALIDATED` entry is never published.  Its loader checks for invalidation
before starting backing I/O and again before publication; it skips or discards
the result accordingly.  Waiters retry lookup against the current inode size
after the invalidated entry retires, rather than observing stale bytes or a
spurious I/O error.

An `ERROR` entry is removed from future hash lookups but retains its saved
error until all current waiters have observed it and released their entry
references.  This gives every waiter on one failed load the same result while
a later independent lookup is free to attempt a fresh load.  The failed page
is freed and its reserved cache-budget slot is released before the error entry
waits for its remaining metadata references to drain.

No I/O, userspace copy, or wait occurs while holding the filemap global lock.
An invalidation can mark a loading entry `INVALIDATED`, but cannot free it
until the loader and waiters drop their entry references.

### EOF Rules

- `generic_file_read()` clamps to EOF and never asks for a wholly beyond-EOF
  page.
- A page straddling EOF loads only its valid prefix; its tail remains zero.
- Preserve existing mmap behavior: a mapping fault wholly beyond EOF receives
  a cached zero page without calling `read_page`, so a shared writable mapping
  can extend the file through existing writeback.

### Capacity and Reclaim

Use an initial cache budget of 8,192 pages (32 MiB) for the 256 MiB guest.
Reserve the budget at `LOADING` creation.  Release it on failed load or LRU
reclamation; if invalidation races a load, release it only when the invalidated
entry retires after its loader and waiters drop their entry references.

Only evict an `UPTODATE` entry held solely by the cache.  Never evict loading,
writeback, or PTE-mapped pages.  If no victim exists, `filemap_get_page()`
returns `-ENOSPC`.

The normal-read helper handles only `-ENOSPC` by loading through a temporary
uncached zero page and freeing it immediately after the copy.  mmap faults do
not have that fallback because they need a stable PTE-backed page.  Neither
path converts a real allocation failure or filesystem error into a cache
capacity fallback.

## Read, Write, and mmap Coherence

### Normal Read

Route regular-file `.read` for ext2, MarioFS, and FAT to
`generic_file_read()`.  It acquires one cache page at a time, copies the
requested subrange with `memcpy_tofs()`, updates `f_pos`, and releases the
temporary page reference.  It preserves current EOF and short-read behavior:
after copying bytes from earlier pages, a later load failure returns the short
count; otherwise it returns the error.

### Normal Write and MAP_SHARED Writeback

Filesystem writes remain authoritative for backing storage.  Each existing
filesystem write loop already uses a stable kernel block buffer before its
block write.  After a successful backing write, it calls
`filemap_update_cached_range()` with the successfully committed bytes from
that kernel buffer.

The helper updates existing cached pages only; it does not allocate or load a
page because of a write.  This avoids rereading a mutable user buffer and makes
direct writes visible through an existing MAP_SHARED mapping.  Short or failed
writes update only the bytes known to have reached the filesystem.

Hold `inode->i_sem` from a cache-miss loader's size/layout snapshot through
its final page publication or retirement.  Likewise, each filesystem holds
`inode->i_sem` from its backing write or truncate through the matching cached
range update or invalidation.  Therefore, if a write-side update sees a
`LOADING` entry, it must not wait for it while holding `inode->i_sem`: that
loader has not yet acquired the semaphore and will read the completed write
afterward.  If a loader already owns the semaphore, it publishes before the
writer can begin.  This prevents stale-page publication and an i_sem/entry
wait deadlock.

MAP_SHARED writeback follows the same sequence: copy page bytes into the
filesystem's kernel block buffer, complete the backing write, then update any
matching cache page with those committed bytes.  Updating the source cache
page with identical bytes is harmless.

`WRITEBACK` prevents eviction only; it is not an exclusive content lock.
Readers may continue to use an up-to-date page.  A later direct writer has
already serialized its backing I/O through `inode->i_sem`, so its cached-range
update may take the entry lock and update a `WRITEBACK` entry without waiting
for the writeback flag to clear.  This preserves backing-write order while
avoiding an unnecessary i_sem-to-writeback wait.

Keep current MAP_PRIVATE/COW semantics: a private copied page is not updated
by a direct file write.  The cache adds no dirty-page list or writeback daemon;
the existing PTE dirty-bit and `filemap_sync()` paths remain responsible for
shared-mapping writeback and retain their current failure semantics.

### Truncate, Extension, and Inode Lifetime

- On shrink, zero the retained partial-page tail and remove entries at or past
  the new EOF.  Already-present PTEs are not revoked by this project;
  `filemap_sync()` continues to suppress writeback when their entry is gone.
- On extension, retain cached beyond-EOF zero pages instead of dropping them:
  a pre-existing MAP_SHARED PTE may still reference one.  Zero newly exposed
  bytes in the old-EOF page, then let `filemap_update_cached_range()` replace
  the successfully written subranges in any retained pages.
- Split the current one-size-fits-all growth/shrink invalidation behavior if
  necessary: only shrink removes cache entries.  Growth preserves zero pages
  so a later direct write cannot leave an existing mapping observing stale
  zeros.
- `filemap_invalidate_pages()` and `filemap_invalidate_inode()` must invalidate
  both ready and loading entries without waiting under the inode-cache lock.
- Idle cache entries do not retain inode references; invalidation removes them
  before an inode structure can be recycled.

## Locking Rules

Lock ordering is:

```text
inode->i_sem -> filemap global lock -> entry lock
```

The global lock protects lookup, state transitions, references, budget, and
LRU only.  The loader releases it before taking `inode->i_sem` and I/O.
MAP_SHARED writeback marks an entry `WRITEBACK`, drops its entry lock, invokes
the filesystem writer, then reacquires the entry lock to finish.  It must not
hold an entry lock while invoking `.write`, because direct filesystem writes
take `inode->i_sem` and then update cache entries.

Concurrent read and write byte visibility follows existing filesystem/POSIX
semantics.  The plan prevents stale cache contents after a completed write; it
does not promise atomic snapshots to a reader racing an unrelated writer.

## Filesystem Integration

### ext2

- Add an ext2 `read_page` using `ext2_bmap()` and `ext2_read_block()`.
- Support every ext2 block size the kernel accepts: 1 KiB, 2 KiB, and 4 KiB.
  The loader reads one, two, or four ext2 blocks respectively for a page and
  preserves sparse-hole zero filling.  The Python image happens to use 4 KiB,
  so its hot path uses one ext2 block per page.
- Route `ext2_file_read` through `generic_file_read` and update cached ranges
  from its existing kernel block buffers after successful writes.

### MarioFS

- Add a `read_page` that walks MarioFS data blocks and uses `bread()`.
- Route ordinary reads through `generic_file_read`.
- Keep the 512-byte MarioFS buffer cache beneath the page loader.
- Update cached ranges only after successful buffered-block writeback.

### FAT

- Factor the existing FAT file-data read loop into `read_page`.
- Route ordinary reads through `generic_file_read`.
- Retain the FAT metadata cache; the file page cache does not replace it.
- Update cached ranges after successful FAT data writes.

## Observability

Add `/sys/kernel/debug/filemap`.

- `cat /sys/kernel/debug/filemap` reports counters only.
- `echo reset > /sys/kernel/debug/filemap` resets counters only.
- Neither changes cache contents, page references, or LRU order.

Report at least:

- normal-read and mmap acquisitions, hits, misses, and cross-path hits;
- loaders started, waiters, loader errors, and invalidations;
- uncached loader bytes by filesystem;
- temporary normal-read fallbacks; and
- evictions and budget-full events.

These counters establish whether Python has enough duplicated file-page I/O
for the unified cache to materially reduce AHCI commands.  They are separate
from, and do not change, the AHCI profiler.

Set an entry's fill source when its load completes and retain it until that
entry is removed.  A normal-read hit on an mmap-filled entry increments the
read-from-mmap counter; an mmap hit on a normal-read-filled entry increments
the mmap-from-read counter.  Hits on beyond-EOF zero pages are reported
separately and do not count as backing-I/O reuse.

## Delivery Order

1. Add focused cache-counter tests for repeated reads, read-then-mmap,
   mmap-then-read, and concurrent same-page misses.
2. Implement cache entry state, references, LRU/budget, invalidation, and
   debugfs counters.
3. Add `read_page` loaders for ext2, MarioFS, and FAT, then move generic mmap
   faults to `filemap_get_page()` and require `read_page` from every regular
   filesystem that supports mmap.  Public ordinary reads remain unchanged at
   this stage; pass all existing mmap tests.
4. Add `generic_file_read()` and convert ext2.  Validate ext2 ordinary-read,
   mmap, and cache-counter semantics.
5. Add cached-range updates for ext2 ordinary writes and MAP_SHARED writeback.
   Validate ext2 write, truncate, and direct-write/MAP_SHARED coherence.
6. Convert MarioFS and FAT ordinary reads and writes, preserving their
   lower-level metadata/buffer caches, then validate the same semantics for
   each filesystem.
7. Profile `python-0`; use filemap and AHCI counters to choose any follow-up.

## Validation and Performance Gate

Run focused tests while implementing:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 1 --jobs 1 \
  tests/shell/ext2.mmap.test \
  tests/shell/ext2.mmap.stress.test \
  tests/shell/ext2.mount.test \
  tests/shell/ext2.write.test \
  tests/shell/mariofs.mmap.test \
  tests/shell/mariofs.mmap.stress.test \
  tests/shell/mariofs.perf.test \
  tests/shell/vfat.mmap.test \
  tests/shell/vfat.mmap.stress.test \
  tests/shell/vfat.write.test
```

Add ordinary-read/write cache tests for MarioFS and cache-counter assertions
for all three filesystems.  Required edge cases are: failed page load, two
simultaneous misses, invalidation during load, full cache, partial EOF page,
short write, direct write visible through MAP_SHARED, and private mapping not
updated by direct write.  Also map a page beyond EOF, extend the file with a
direct write into that page, and verify the existing MAP_SHARED mapping sees
the committed bytes.  For a truncated mapped page, verify that later unmap or
sync does not write the invalidated page back; PTE revocation itself is outside
this plan's scope.

Then run the traced Python test at least three times and compare medians:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 1 --jobs 1 \
  tests/shell/python-0.test
```

Capture both `/sys/kernel/debug/filemap` and `/sys/kernel/debug/ahci`.  If
cross-path page reuse is low, do not claim a large Python win from this cache:

- predominantly ext2 indirect-pointer reads justify a bounded ext2 metadata
  cache next;
- predominantly compulsory data reads justify contiguous multi-block loading
  or controlled readahead next.

Finally run:

```sh
make test-all
```

Completion requires all tests to pass and no new filesystem or mmap
regressions.  A faster `python-0` result does not compensate for correctness
failures.
