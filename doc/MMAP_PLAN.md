# File-Backed mmap for ext2, FAT32, and MarioFS

## Goal

Fully implement regular-file `mmap` for the three writable filesystems in
Mario: MarioFS, ext2, and FAT32. Both `MAP_PRIVATE` and `MAP_SHARED` mappings
must work for read and, where the file descriptor permits, write access.
`msync` must flush shared mappings synchronously, and `munmap`/process exit
must flush dirty shared pages without dropping the backing file while the
mapping is alive.

The generic VFS path is the right place for this work. All three filesystems
already have working regular-file `.read` and `.write` implementations, so the
mmap layer can read and write pages through those operations instead of adding
three filesystem-specific mmap backends.

## Status

The pieces exist but the file-backed path is incomplete:

- `do_mmap()` reaches `file->f_op->mmap` in `src/mm/mm.c`.
- `generic_file_mmap()` exists in `src/mm/filemap.c`.
- Only MarioFS wires `.mmap = generic_file_mmap` in
  `src/fs/mariofs/file.c:308`.
- ext2 (`src/fs/ext2/file.c`) and FAT32 (`src/fs/fat/file.c`) have `.read` and
  `.write` but no `.mmap`.
- `SYS_msync` is currently `sys_not_exist` in `src/kernel/entry.S:525`.

The current generic implementation is not reliable enough to simply enable on
ext2 and FAT32. It needs the fixes described below first.

## Current Architecture and Limitations

### VMA lifetime

`struct vm_area_struct` in `src/include/mm/mm.h` stores `vm_inode` but not the
`struct file` used to create the mapping. `generic_file_mmap()` only does
`iref(inode)`. Closing the original fd therefore leaves the mapping with no
safe file reference, and callbacks that need `f_op`/`f_mode` have to
manufacture a synthetic `struct file`.

### Page-in path

`filemap_nopage()` in `src/mm/filemap.c`:

- synthesizes a stack `struct file`,
- assumes `i_fop` and `.read` are non-null,
- treats a short or failed read as success,
- does not handle pages beyond EOF,
- passes a kernel page buffer to filesystem read code that uses
  `memcpy_tofs()` with the current user `%fs`, which is incorrect for kernel
  buffers.

### Fault/COW path

`do_no_page()` in `src/mm/fault.c` installs the returned page with
`vma->vm_page_prot`. For a private writable file mapping this creates a
writable PTE immediately, so MAP_PRIVATE COW semantics are only enforced after
a later `fork`. `do_wp_page()` has generic COW for private mappings and can be
reused.

### Writeback path

`filemap_sync_pte_range()` and `filemap_sync_pde_range()` mix absolute
addresses with relative lengths, do not iterate page tables correctly across
pde boundaries, and write every present page rather than tracking dirtiness.
`filemap_sync()` also passes a kernel page through the filesystem `.write`
path, which uses `memcpy_fromfs()` and therefore has the same `%fs` problem as
the read path.

### Missing syscall

`msync` is absent from the syscall table. Without it there is no explicit way
to force shared mapping data to disk before reading the file again with
`read()`.

## Target Semantics

Implement these first:

- `MAP_PRIVATE | PROT_READ`
- `MAP_PRIVATE | PROT_READ | PROT_WRITE`
- `MAP_SHARED | PROT_READ`
- `MAP_SHARED | PROT_READ | PROT_WRITE`
- `PROT_EXEC` is accepted as read on i386 because the current page-table model
  has no NX bit.
- `MAP_FIXED` and `MAP_ANONYMOUS` must keep their current behavior and must not
  regress.
- `mmap`/`mmap2` offset must be page aligned; mapping length is rounded up to a
  page by the existing VMA layer.
- Closing the original fd after a successful mapping must not invalidate the
  mapping.

File-backed behavior:

- Reads inside the file return file bytes.
- Reads in the partial page containing EOF return file bytes followed by
  zeroes.
- Reads wholly beyond EOF return zeroes for private mappings and for
  read-only/read faults. Shared writable mappings may later extend the file on
  writeback.
- Private writes never modify the backing file.
- Shared writes are written back by `msync(MS_SYNC)`, `munmap`, `exit`, or
  `exec`.
- Shared writes past EOF extend the file through the filesystem's existing
  write/extension path.
- Shared mappings must flush before the page table entries are dropped.

Explicitly defer unless they become required:

- `mremap`.
- Full `mprotect` semantics. The current `sys_mprotect()` is still a no-op;
  this plan only preserves it.
- `mlock`/`munlock`.
- Demand paging eviction under memory pressure. Pages may remain cached for
  the lifetime of the inode/mapping.
- Async writeback. `MS_ASYNC` can be implemented as synchronous flush in the
  first version because there is no background flusher.

## Design

### 1. VMA file ownership

Add a `struct file *vm_file` field to `struct vm_area_struct` in
`src/include/mm/mm.h`. Keep `vm_inode` as a non-owning convenience pointer for
code that only needs the inode, or replace its ownership semantics entirely.
The invariant is:

- `vm_file != NULL` for file-backed VMAs.
- `vm_file == NULL` for anonymous VMAs.
- `vm_file` owns one `struct file` reference obtained with `fref()`.
- `put_file(vm_file)` releases that reference when the VMA is destroyed.
- No separate `iref(vm_inode)` for file-backed VMAs; the file reference keeps
  the inode alive.

Update every VMA copy/destroy path to use `vm_file`:

- `generic_file_mmap()`: `fref(file)` and record `vm_file`.
- `dup_mmap()`: `fref(tmp->vm_file)` for copied file VMAs.
- `exit_mmap()`: call VMA close, then `put_file(vma->vm_file)`.
- `unmap_fixup()`/`do_munmap()`: fref when splitting/copying a VMA, put when
  destroying the original, and ensure a full unmap does not both `put_file()`
  and `iput()` the same backing inode.
- `merge_mmap()`: use `vm_file` (or `vm_inode`) as the file-backed guard so
  anonymous VMAs can still merge.

`do_mmap()` already validates fd permissions against `file->f_mode`. Keep that
check. Add an error path in `do_mmap()` that releases `vm_file` if
`file->f_op->mmap()` fails after taking its reference.

### 2. Page-in helper

Replace the synthetic-file read in `filemap_nopage()` with helpers that call
the filesystem with an explicit file position and do not move the original
file's `f_pos`.

```c
static int filemap_read_page(struct file *file, unsigned long pos,
                             unsigned long page, unsigned long bytes);
```

The helper copies the `struct file` to a stack variable, sets `f_pos = pos`,
`f_mode = 1`, and `f_flags = 0`, then calls `f_op->read` with `%fs` temporarily
set to `KERNEL_DS` because filesystem read implementations use
`memcpy_tofs()`. Restore `%fs` before returning.

`filemap_nopage()` must:

- Compute `pos = vma->vm_offset + (addr & PAGE_MASK) - vma->vm_start`.
- Start from a zero page, so any bytes not read are already zero.
- If `pos >= inode->i_size`, leave the page zeroed and return success.
- If the page straddles EOF, read only the bytes through EOF and leave the
  tail zeroed.
- Require the read helper to return the full requested byte count. A short
  read without reaching EOF is an I/O error.
- Propagate errors distinctly: allocation failure is OOM, file I/O failure is
  `SIGBUS`, and ordinary bad-access faults remain `SIGSEGV`.

The existing `get_shared_page()` share-ring optimization remains useful for
forked shared VMAs in the short term. If a shared page already exists, reuse it
after taking a page reference.

### 3. Private COW and PTE protection

In `do_no_page()`, after `vm_ops->nopage()` fills a file page:

- For `MAP_SHARED | PROT_WRITE`, install the PTE with `PG_RW`.
- For `MAP_PRIVATE | PROT_WRITE`, install the PTE read-only even if the VMA
  `vm_page_prot` contains `PG_RW`. The first write then takes the existing
  private `do_wp_page()` COW path.

This gives correct MAP_PRIVATE write isolation before and after fork. No
filesystem-specific `wppage` hook is needed because the generic COW already
copies the page and uses the VMA's page protection.

### 4. Shared writeback

Rewrite `filemap_sync`, `filemap_sync_pde_range`, and
`filemap_sync_pte_range` around an absolute `[start, end)` range. Iterate page
by page, recalculate the pde/page-table entry at each step, and stop cleanly at
pde and VMA boundaries.

Use the x86 PTE dirty bit (`PG_DIRTY`) to avoid writing clean pages. Add
helpers such as `__pe_dirty()` and `clear_pe_dirty()` to
`src/include/mm/pagetable.h`. On successful writeback clear the current PTE's
dirty bit. A shared page referenced from another mapping may be written again
by that mapping later; that is safe and only costs an extra write.

Add a `filemap_write_page()` helper symmetric to `filemap_read_page()`:

- copies the `struct file`,
- sets `f_pos` to the file position,
- sets `f_mode = 2` and `f_flags = 0` so `O_APPEND` never affects writeback,
- sets `%fs = KERNEL_DS` around `f_op->write`.

The filesystem `.write` methods already take `inode->i_sem` for ext2, FAT32,
and MarioFS, so writeback is serialized against ordinary writes and truncates.
Do not call `.write` while holding that inode semaphore in the mmap layer.

`filemap_unmap()` flushes the range being removed before
`unmap_page_range()` drops the PTEs. `filemap_close()` flushes the whole VMA on
process exit or full unmap. Adjust the `do_munmap`/`unmap_fixup` ordering so
the flush always happens while the page tables are still present. Duplicate
writes caused by both `unmap` and `close` seeing clean pages are harmless.

### 5. msync syscall

Implement `sys_msync(unsigned long addr, unsigned long len, int flags)`:

- Validate `addr` page alignment and the address range.
- Accept `MS_SYNC`, `MS_ASYNC`, and `MS_INVALIDATE`; reject unknown flags and
  conflicting `MS_SYNC | MS_ASYNC`.
- Treat `MS_ASYNC` as synchronous writeback for now.
- Treat `MS_INVALIDATE` as accepted but a no-op until there is a more complete
  page cache.
- Iterate every VMA intersecting the range, call `vm_ops->sync()` for shared
  writable file VMAs, and return `-ENOMEM` if any part of the range is
  unmapped.
- Make `msync` on private mappings a no-op success.

Wire it into `src/kernel/entry.S` in place of `sys_not_exist`. The trace name
already exists in `src/kernel/syscall_trace.c`.

### 6. Filesystem wiring

Add `.mmap = generic_file_mmap` to:

- `src/fs/ext2/file.c` (`ext2_file_fops`)
- `src/fs/fat/file.c` (`fat_file_fops`)

MarioFS already has it. After wiring, verify each filesystem's `.read` and
`.write` satisfy the mmap helper assumptions:

- ext2 read zero-fills holes and write allocates blocks.
- FAT32 read/write resolves cluster chains and write already handles write-past-
  EOF and readonly mounts.
- MarioFS read/write follows its block chain and write extends the file.

For read-only FAT32 mounts, `MAP_SHARED | PROT_WRITE` should fail at either
open time or mmap time. Add an explicit readonly superblock check in
`generic_file_mmap()` if open does not already reject it reliably.

### 7. Cross-mapping coherence

The current share ring only shares pages between parent and child after fork.
Correct `MAP_SHARED` behavior across two independent `mmap` calls, especially
across processes, requires a small file page cache. Add a per-inode page-cache
list plus a global lookup keyed by `(inode, page_index)`.

A cache entry owns one page reference; each installed PTE owns another. On
page-in, look up the page first, and only allocate/read when missing. Shared
and private mappings can use the same clean cache page; private write faults
COW as above. Cache entries are invalidated on truncate and released when the
inode is finally evicted. For writeback, shared VMAs still flush their present
pages through the same `filemap_write_page()` helper, so no dirty reverse-map
is required in the first version.

Filesystem truncate hooks must call `filemap_invalidate_pages(inode, from)`:

- `ext2_truncate()` in `src/fs/ext2/file.c`
- `fat_file_truncate()` in `src/fs/fat/file.c`
- `mario_file_truncate()` in `src/fs/mariofs/file.c`

Invalidate/zero pages beyond the new EOF so a later writeback cannot resurrect
stale data.

## Milestones

Each milestone should be committable independently.

### Milestone 1: VMA file lifetime and syscall scaffolding

- Add `vm_file` and all copy/destroy reference handling.
- Update `generic_file_mmap()` to `fref(file)` and validate regular-file plus
  mapping permissions.
- Add a minimal `sys_msync` registration that validates arguments and returns
  `-ENOSYS`/success without writeback.
- Wire `.mmap` into ext2 and FAT32.
- Verify anonymous mmap and fork do not regress.

### Milestone 2: Correct read page-in and MAP_PRIVATE

- Rewrite `filemap_nopage()` with explicit file position, EOF handling, and
  the `KERNEL_DS` read helper.
- Adjust `do_no_page()` so private writable file pages are installed
  read-only.
- Add the error distinction in the no-page path.
- Implement read-only and MAP_PRIVATE read/write tests for MarioFS, ext2, and
  FAT32.

### Milestone 3: Shared writeback and msync

- Rewrite the filemap sync functions with correct range iteration and PTE
  dirty-bit checks.
- Add `filemap_write_page()` with the `KERNEL_DS` write helper.
- Implement `sys_msync` over VMAs and wire the syscall table.
- Fix `munmap`/`unmap_fixup` flush ordering.
- Test shared write + `msync` + `munmap` + process exit for all three
  filesystems.

### Milestone 4: Coherent page cache and truncate invalidation

- Add a small file page cache keyed by inode and page index.
- Replace the file-map share-ring lookup with cache lookup for independent
  mappings; keep COW for private mappings.
- Add inode invalidation hooks to ext2, FAT32, and MarioFS truncate paths.
- Add cross-mapping and cross-process shared-mapping tests.

### Milestone 5: Hardening and full validation

- Add write-past-EOF, partial last page, `MAP_FIXED`, offset validation,
  readonly FAT32, fd-close-after-mmap, fork COW, and truncate interaction
  tests.
- Run `make test` plus the targeted filesystem shell tests.
- Document remaining mmap semantics in `doc/MMAP_PLAN.md` and update
  `doc/FAT32_FILESYSTEM_PLAN.md` to remove mmap from its deferred list.

## Test Plan

Add a dedicated userspace test program, for example extending
`app/test/mm/main.c` or adding `app/test/filemmap`. It should accept a path
and a mode and use libc `mmap`, `msync`, `munmap`, `fork`, and `read`/`write`.

Core cases per filesystem:

- MAP_PRIVATE read matches file content.
- MAP_PRIVATE write changes the mapping but not the backing file.
- MAP_PRIVATE write does not become visible in the parent/child COW case.
- MAP_SHARED read matches file content.
- MAP_SHARED write is visible after `msync(MS_SYNC)`.
- MAP_SHARED write is visible after `munmap` and after process exit.
- Mapping past EOF reads zeroes in the tail.
- Shared write past EOF extends the file.
- Closing the fd after mmap still allows reads and shared writeback.
- Partial munmap flushes only unmapped dirty pages without crashing or losing
  other pages.

Shell tests:

- `tests/shell/mariofs.mmap.test`: no HD required, uses the MarioFS root.
- `tests/shell/ext2.mmap.test`: `REQUIRES-HD:` and uses `/dev/hdc3`.
- `tests/shell/vfat.mmap.test`: `REQUIRES-HD:` and uses `/dev/hdc2`, with
  QEMU `-snapshot`.
- `tests/shell/mariofs.mmap.stress.test`: staged shared/private read-write,
  truncate/growth, and append workloads on the MarioFS root.
- `tests/shell/ext2.mmap.stress.test`: the same stress stages on ext2.
- `tests/shell/vfat.mmap.stress.test`: the same stress stages on FAT32.

Run:

```sh
make
make test
.venv/bin/python tests/lit_shell_test.py --test ext2.mmap.test tests/shell
.venv/bin/python tests/lit_shell_test.py --test vfat.mmap.test tests/shell
.venv/bin/python tests/lit_shell_test.py --test mariofs.mmap.test tests/shell
```

Adjust the exact lit runner command to the repository's current test harness
if it has changed.

## Risks

- The filesystem `.read`/`.write` methods use `memcpy_tofs()`/`memcpy_fromfs()`
  and therefore depend on the mmap helpers setting `KERNEL_DS`; forgetting to
  restore `%fs` would corrupt ordinary syscall copies.
- VMA splitting already has subtle use-after-free history. Adding `vm_file`
  must update every split/copy path exactly once or the kernel will drop or
  leak file references.
- Page-cache invalidation must be added to all three truncate paths. Missing
  one filesystem can make stale data reappear after a truncate.
- FAT32 readonly mounts and rollback paths must not be bypassed by writeback.
- Holding `inode->i_sem` during page-in is acceptable, but the mmap layer must
  not call `.write` while holding it because the filesystem write methods take
  it again.
- The existing `do_wp_page()` uses `vm_page_prot` when copying. Ensure private
  file pages keep the intended protection so COW copies are writable while
  original pages remain read-only.
