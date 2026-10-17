# Debugging the Python `subprocess.run(['pwd'])` segmentation fault

This document describes how the intermittent `tests/shell/python-1.test`
failure was reproduced, narrowed down, and fixed.

## 1. Symptom

Run the single shell test:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell/python-1.test
```

The test starts Python in the Mario shell and executes:

```python
import subprocess
subprocess.run(['pwd'])
```

Before the fix, repeated runs could fail in two ways:

- `pexpect.exceptions.TIMEOUT` waiting for the Python `>>>` prompt.
- The QEMU serial console showed:

  ```text
  /
  Segmentation fault
  ```

The failure was intermittent, which already pointed to a kernel memory
management or scheduling problem rather than a deterministic Python bug.

## 2. First narrowing step: CPython vfork fast path

CPython's `subprocess` module defaults to `_USE_VFORK = True`. One early
experiment disabled that fast path:

```python
_USE_VFORK = False
```

That made the symptom less frequent in some runs, but did not reliably fix
it. This was important evidence: the vfork path was a trigger, but the
real bug was in the kernel fork/exec/exit memory path.

## 3. GDB evidence

Run QEMU with a GDB stub:

```sh
cd qemu
make quick-debug
```

In another terminal, attach GDB to the kernel:

```sh
gdb -q src/kernel/kernel.exe
```

```gdb
target remote :1234
set pagination off
```

Instead of stepping through Python, use breakpoints as an event logger at
the kernel boundaries that matter for:

```text
fork -> child exec/exit -> parent resume
```

The important breakpoints are:

```gdb
b src/kernel/fork.c:115
commands
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "FORK cur=%d vfork_arg=%d child=%d\n", $cur->pid, vfork, p->pid
continue
end

b src/kernel/exit.c:208
commands
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "EXIT_NOTIFY pid=%d code=%d parent=%d\n", $cur->pid, $cur->exit_code, $cur->p_pptr->pid
continue
end

b src/mm/fault.c:186
commands
silent
set $cur = (struct task_struct *)((unsigned long)$esp & ~8191UL)
printf "BAD_FAULT pid=%d addr=%x eip=%x err=%d\n", $cur->pid, addr, $eip, error_code
continue
end
```

Continue and reproduce the Python `subprocess.run(['pwd'])` command.

The failing sequence was consistently:

```text
FORK cur=14 vfork_arg=0 child=15
EXIT_NOTIFY pid=15 code=0 parent=14
BAD_FAULT pid=14 addr=36000 eip=5b9c9e err=4
BAD_FAULT pid=14 addr=400c8dc0 eip=8010f184 err=2
```

Interpretation:

- Process 14 forks child 15.
- Child 15 exits normally, so the child did not die.
- Parent 14 faults after the child exits, not during `fork()` or `exec()`.
- The first fault is user mode at a normal Python EIP, but uses an invalid
  Python object address (`0x36000`).
- The second fault is a kernel-mode write fault (`err=2`), meaning kernel
  memory was already corrupted.

Resolve the user EIP:

```sh
addr2line -e app/usr/Python-3.13.13-install-mario/bin/python3.13 0x5b9c9e
```

It points into `_PyEval_EvalFrameDefault`. Python is not executing a bad
syscall; its heap/object memory has been corrupted.

This narrows the scope from Python or `subprocess` to kernel memory
teardown after child exit. The relevant path is:

```text
do_exit()
  free_page_tables()
    free_one_pde()
      free_pt()
```

That path became the focus of the static inspection in the next section.

## 4. Suspect code: page-table release

The kernel uses copy-on-write page tables:

- `alloc_pt()` returns a zeroed page table as a **kernel virtual address**.
- `free_one_pde()` in `src/mm/paging.c` obtains that same virtual address:

  ```c
  pt = pte_offset(__vir(*pde), 0);
  set_pde(pde, 0);
  ...
  free_pt(pt);
  ```

The old definition in `src/include/mm/pagetable.h` was:

```c
#define free_pt(pt) __put_page(PHY_TO_PAGE(pt))
```

`PHY_TO_PAGE()` expects a **physical** address. Passing a kernel virtual
address caused `__put_page()` to operate on an out-of-bounds `struct page`.

More precisely:

```c
#define PHY_TO_PAGE(x) PFN_TO_PAGE((unsigned long)(x) >> PAGE_SHIFT)
#define VIR_TO_PAGE(x) PHY_TO_PAGE(__phy(x))
```

For a page-table virtual address such as `0x801xxxxx`, `PHY_TO_PAGE()`
computed a page-frame number of about `0x80100`, producing a pointer far
past the real `mem_map` array. The wrong `struct page` count was decremented,
corrupting nearby kernel memory, while the actual page-table page was leaked.

This explains the intermittent Python corruption after child exec/exit:
Python and its `pwd` child create and destroy user page tables, and each
destroy path hit the bad conversion.

## 5. Fix

Change `free_pt()` to convert the virtual address through `__phy()` first:

```diff
--- a/src/include/mm/pagetable.h
+++ b/src/include/mm/pagetable.h
@@
-#define free_pt(pt)	__put_page(PHY_TO_PAGE(pt))
+#define free_pt(pt)	__put_page(VIR_TO_PAGE(pt))
```

The only `free_pt()` call site is in `src/mm/paging.c` and passes a kernel
virtual address, so `VIR_TO_PAGE()` is the correct conversion.

## 6. Rebuild and restore the normal Python path

Rebuild the kernel:

```sh
make -C src \
  CC="$PWD/util/mario-cc" \
  LD="$PWD/util/mario-ld"

cp src/kernel/kernel.exe qemu/tmp/kernel.exe
```

Make sure Python still uses its normal vfork fast path. In the Python source
and the Mario install tree, keep:

```python
_USE_VFORK = True
```

There is no longer a reason to disable it.

If the ramdisk needs to be refreshed:

```sh
make rd
cp rd/rdx qemu/tmp/rdx
```

## 7. Verify

Run the originally failing test repeatedly:

```sh
for i in $(seq 1 30); do
  .venv/bin/python tests/lit_shell_test.py tests/shell/python-1.test || exit 1
done
```

Expected result: all 30 runs pass.

Then run the complete shell suite:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell
```

Expected result:

```text
PASS: tests/shell/fs.test
PASS: tests/shell/oksh.test
PASS: tests/shell/python-0.test
PASS: tests/shell/python-1.test
```

## 8. Summary

Root cause: `free_pt()` fed a kernel virtual address to `PHY_TO_PAGE()`,
which expects a physical address.

Effect: every page-table teardown decremented the wrong `struct page`,
corrupting kernel memory and leaking the real page-table page.

Fix: use `VIR_TO_PAGE()` in `free_pt()`.
