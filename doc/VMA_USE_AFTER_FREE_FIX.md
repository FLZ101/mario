# VMA Use-After-Free Fix

## Problem

The MarioFS full-block-map change exposed a latent use-after-free in the VM
area split path. The benchmark repeatedly crashed with a user-mode
segmentation fault while Python was faulting executable pages into memory.

The corrupted page contained valid Python code immediately after
`do_no_page()` mapped it, but was overwritten before the CPU executed it.
The overwrite came from `remove_shared_vm_struct()` writing through a stale
`vm_next_share` / `vm_prev_share` pointer.

## Root Cause

`struct vm_area_struct` contains a share ring:

```c
struct vm_area_struct *vm_next_share;
struct vm_area_struct *vm_prev_share;
```

Freshly created VMAs are self-linked:

```c
vma->vm_next_share = vma->vm_prev_share = vma;
```

When `do_munmap()` unmaps only part of a VMA, it:

1. removes the original VMA from the process `mmap` list,
2. calls `remove_shared_vm_struct(vma)`, which unlinks the original VMA and
   then sets its share pointers back to itself,
3. calls `unmap_fixup()`,
4. frees the original VMA with `kfree(vma)`.

`unmap_fixup()` keeps the remaining part of the mapping by allocating a new
VMA and copying the original into it:

```c
*mpnt = *vma;
```

That copy includes the original VMA's share pointers. After step 2, those
pointers both point to the original VMA object:

```c
mpnt->vm_next_share == vma;
mpnt->vm_prev_share == vma;
```

`do_munmap()` then frees `vma`, but the new `mpnt` is still linked to it
through `vm_next_share` and `vm_prev_share`.

Later, when the new VMA is itself unmapped, `remove_shared_vm_struct()` does:

```c
vma->vm_next_share->vm_prev_share = vma->vm_prev_share;
vma->vm_prev_share->vm_next_share = vma->vm_next_share;
```

Those writes land in memory that used to contain the freed VMA. If that
kmalloc object or page has been reused, this corrupts unrelated data.

In the failing benchmark, the freed VMA's backing page was reused as a
zero page for a Python executable demand page. The stale share-ring write
then modified bytes inside that executable page, and Python faulted when
the corrupted instruction stream was reached.

## Why The Full Block Map Exposed It

The full block map changes MarioFS allocation and read patterns, which in
turn changes kmalloc/page reuse timing. The VMA bug already existed, but the
old contiguous-block cache happened to avoid the particular allocation and
fault sequence that turned the stale pointer into visible executable-page
corruption.

## Fix

After copying the original VMA in `unmap_fixup()`, relink the new VMA's
share ring to itself before inserting it into the process `mmap` list:

```c
*mpnt = *vma;
mpnt->vm_next_share = mpnt;
mpnt->vm_prev_share = mpnt;
```

This is done in both places where `unmap_fixup()` creates a replacement VMA:

- the middle-split case that creates the right-hand remainder,
- the common case that creates the final left/right remainder.

The original VMA is unlinked from its share ring before `unmap_fixup()`, so
self-linking the split pieces is consistent with a newly created mapping and
never leaves a pointer to the soon-to-be-freed original.

## Verification

Reproduce with:

```sh
make CC=$PWD/util/mario-cc LD=$PWD/util/mario-ld -C src -j2
.venv/bin/python tests/lit_shell_test.py --test mariofs.perf.test tests/shell
```

Before the fix, the benchmark could segfault while Python demand-paged its
executable. After the fix, the benchmark passes and the VGA tests also pass.
