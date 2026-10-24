# SMP and Multithreading Support Plan

## Goal

Extend Mario from the current single-CPU, single-runqueue kernel to:

- Detect and boot multiple x86 processors.
- Schedule tasks on more than one CPU.
- Expose CPU affinity through the existing `sched_setaffinity` and
  `sched_getaffinity` syscalls instead of hard-coding CPU 0.
- Implement enough `clone` and thread-group support for userspace
  multithreading, including `CLONE_VM`, `CLONE_FILES`, `CLONE_FS`,
  `CLONE_SIGHAND`, and `CLONE_THREAD`.
- Keep the current single-CPU path working when only one CPU is present or no
  usable APIC/MP/MADT information exists.

The work is ordered so every step leaves the tree bootable and testable.  The
steps below replace the older coarse milestone list.  Each step has an
objective, the kernel changes it makes, and concrete correctness checks that
must pass before the next step starts.

## Current blockers

The kernel is intentionally UP-only in the following places:

- `src/kernel/sched.c` has one global `runqueue_head`; `schedule()` assumes it
  runs on the only CPU.
- `src/include/task.h` derives `current` from `%esp`, which only works with a
  separate 8 KiB kernel stack and one running CPU.
- `src/kernel/start.S`, `src/kernel/tss.c`, and `src/kernel/tls.c` share one
  GDT/TSS/TLS table.
- `src/kernel/irq.c` initializes only the 8259 PIC and has no local APIC,
  IO-APIC, IPI, MADT, or MP-table code.
- `src/include/lib/spinlock.h`, `atomic.h`, and `bitops.h` use non-locked x86
  instructions; they are safe only while one CPU has interrupts disabled.
- Buddy allocation, `kmalloc`, task lists, wait queues, timers, and file-map
  caches rely on `irq_save()` for mutual exclusion.
- `src/fs/file.c` increments `f_count` under `file_lock`, but decrements it
  without the lock; file-backed mmap `vm_file` references make this a real
  cross-task teardown path.
- `SYS_clone` and `SYS_futex` are mapped to `sys_not_exist`.

## Step 0: SMP-aware test harness

Objective: make it possible to run the same lit tests with one or more QEMU
CPUs without changing the default behavior.

Changes:

- Add `SMP ?= 1` to `qemu/Makefile` and pass `-smp $(SMP)` to QEMU targets.
- Add `--smp N` to `tests/lit_shell_test.py`; keep the default at `1`.
- Add `qemu/Makefile` convenience targets `smp2-run`, `smp4-run`,
  `smp2-debug`, and `smp4-debug`.

Correctness checks:

- `make shell-test` still passes with the default one-CPU QEMU command.
- `make quick-run SMP=2` boots QEMU with `-smp 2` and reaches the shell.  At
  this stage only the BSP is brought up, so the shell may still report one
  online CPU.
- `python3 tests/lit_shell_test.py tests/shell/heredoc.test --smp 2` runs the
  same test without harness changes to the test file.

Commit: `test: add SMP selection to qemu and shell lit runner`

## Step 1: SMP-safe synchronization primitives

Objective: make the low-level primitives correct on multiple CPUs while still
booting only the BSP.

Changes:

- Rewrite `src/include/lib/spinlock.h`:
  - `ACQUIRE_LOCK` uses `lock xchgb`/a locked test-and-set loop and a full
    compiler barrier.
  - `RELEASE_LOCK` uses a locked release-store with a compiler barrier.
  - Add `spin_lock_irqsave(lock, flags)` and
    `spin_unlock_irqrestore(lock, flags)`.
- Rewrite `src/include/lib/atomic.h` so every RMW uses a `lock` prefix.
- Rewrite `src/include/lib/bitops.h` so `set_bit`, `clear_bit`,
  `change_bit`, `test_and_*_bit` use locked bit instructions.

Comments required: explain why `xchg`/plain `add`/plain `bts` are insufficient
once another CPU can observe the same cache line.

Correctness checks:

- `make -C src` builds cleanly with `-Wall -Werror`.
- Boot the normal quick-run target; the shell still starts.
- Run the allocator-heavy existing tests:
  - `tests/shell/mariofs.mmap.stress.test`
  - `tests/shell/ext2.mmap.stress.test`
  - `tests/shell/vfat.mmap.stress.test`
  - `tests/shell/heredoc.test`
- The primitives must not change scheduling behavior on one CPU.

Commit: `x86: make spinlocks, atomics, and bitops SMP-safe`

## Step 2: lock the allocators and shared kernel lists

Objective: replace interrupt-mask-only critical sections in memory and task
management with real spinlocks so the next AP-boot step cannot race them.

Changes:

- Add a `page_alloc_lock` in `src/mm/page_alloc.c` and use
  `spin_lock_irqsave()` around buddy-list operations.
- Add a `kmalloc_lock` in `src/mm/kmalloc.c` and use
  `spin_lock_irqsave()` around bucket/board operations.
- Add a `tasklist_lock` and convert `SET_LINKS`/`REMOVE_LINKS` in
  `src/include/sched.h` to take it.  Keep interrupts disabled while holding
  it until the scheduler is converted.
- Add a `wait_queue_lock` and convert wait-queue insertion/removal/wakeup.
- Convert timer-list protection in `src/kernel/timer.c` from `irq_save()` to
  a real timer lock.

Comments required: each lock documents what data it protects and why interrupt
masking alone is not enough.

Correctness checks:

- `make shell-test` passes.
- Fork/exec and pipe stress remains stable:
  - `tests/shell/oksh-0.test`
  - `tests/shell/heredoc.test`
  - `tests/shell/sbase.yes.test`
- Run an allocator stress program that forks many short-lived children and
  allocates/frees pages.  At this stage this is exercised through the normal
  shell tests; add `tests/shell/smp.alloc.test` only if a focused reproducer
  is valuable.

Commit: `mm, sched: protect allocators and shared task lists with spinlocks`

## Step 3: protect VMA/file-backed mmap and file refcounts

Objective: fix the two remaining shared-state hazards that do not depend on a
second CPU being online.

Changes:

- Add an `mm_lock` to `src/include/mm/mm.h` and initialize it for the initial
  mm.  Wrap VMA-list mutation in `src/mm/mm.c` (`insert_vm_struct`,
  `unmap_fixup`, `do_munmap`, `exit_mmap`, `dup_mmap`) and page-fault VMA
  lookup in `src/mm/fault.c`.
- Add a `filemap_lock` in `src/mm/filemap.c` around lookup, insertion,
  invalidation, and removal of `filemap_page` entries.  Keep lookup and
  refcount acquisition under the same lock.
- Convert `src/fs/file.c` `release_file()`/`put_file()` to decrement
  `f_count` under `file_lock`; `fref()` already does.

Comments required: explain the truncate-vs-page-fault race in `filemap.c` and
the close-vs-unmap race for `vm_file`.

Correctness checks:

- Existing file-backed mmap and truncate tests pass:
  - `tests/shell/mariofs.mmap.test`
  - `tests/shell/ext2.mmap.test`
  - `tests/shell/vfat.mmap.test`
  - `tests/shell/mariofs.mmap.stress.test`
  - `tests/shell/ext2.mmap.stress.test`
  - `tests/shell/vfat.mmap.stress.test`
- Run `tests/shell/ext2.write.test` to exercise truncate invalidation.
- Run `tests/shell/python-0.test` to exercise repeated fork/exec/mmap.

Commit: `mm, fs: serialize VMA and file-backed page-cache access`

## Step 4: per-CPU data and segment-based current

Objective: remove the `%esp`-derived `current` assumption while still running
only the BSP.  This is the last single-CPU-only step.

Changes:

- Add `src/include/percpu.h` with a BSP `this_cpu_info()` accessor.  Later
  steps expand this once APs have their own slots.
- Add `struct cpu_info` in `src/include/smp.h`; this step keeps only `id`,
  `online`, and `current_task`, with later steps adding the runqueue, TSS,
  GDT, and idle fields as those subsystems move per-CPU.
- Reserve a GDT selector for a per-CPU kernel-data segment, set its base on
  BSP boot, and change `get_current()` plus `entry.S:GET_CURRENT` to read
  through `%gs`.  `%fs` is already used by `get_user`/`put_user`, so `%gs` is
  the kernel segment while a task's user TLS selector is preserved in its
  trap frame and restored by `iret`.
- Initialize the BSP `cpu_info` early in `mario()`.

Comments required: explain the i386 segment setup and why `current` is not
task-private when more than one CPU exists.

Correctness checks:

- `make -C src` builds.
- Quick-run reaches the shell and the existing context-switch-heavy tests
  pass:
  - `tests/shell/toybox.timeout.test`
  - `tests/shell/toybox.tty.test`
  - `tests/shell/oksh-0.test`
- A TLS-using test (`tests/shell/toybox.readelf.test` or an existing VGA test)
  still passes, proving context switches preserve the user TLS selector.

Commit: `x86: introduce per-CPU current through a %gs base`

## Step 5: CPU enumeration and AP boot

Objective: detect CPUs and bring application processors into their own idle
loop.  Device interrupts may still be handled by the BSP.

Changes:

- Put detection, APIC MMIO, INIT/SIPI, and bring-up in `src/kernel/smp.c`;
  keep the low-memory start-up code in `src/kernel/smp_trampoline.asm`.
- Enumerate CPUs from the MP floating pointer/table first, then from the
  ACPI MADT (RSDP/XSDT discovered through QEMU `fw_cfg`); the direct QEMU
  `-kernel` path has neither table, so it accepts `mario.smp=N` from the
  Multiboot command line instead.
- Give the trampoline page a temporary identity mapping while APs turn on
  paging, and unmap it again after all APs enter their idle loop so normal
  low-address protection and process page-table copying are unchanged.
- `smp_ap_entry()` installs an AP-local GDT/per-CPU segment and enters a
  halted idle loop; it does not run the scheduler until Step 6.
- Expose online CPUs through `/proc/cpuinfo` so `nproc` can consume it.
- Keep the PIC-only single-CPU path when `mario.smp=N` is absent and no
  MP/ACPI tables are available.

Comments required: document the trampoline's physical/virtual address split
and why APs stay in idle until Step 6.

Correctness checks:

- `make quick-run SMP=2` logs one line per online CPU (`cpu 0`, `cpu 1`) and
  stays stable at the shell.
- `make quick-run SMP=4` logs CPUs 0 through 3 and stays stable.
- `nproc` or `cat /proc/cpuinfo` reports the number of started CPUs.
- With `SMP=1` the kernel keeps the old PIC-only path and does not require MP
  table/APIC presence.
- Add `tests/shell/smp.boot.test`, marked `REQUIRES-SMP: 4`, that asserts
  `/proc/cpuinfo` reports the requested number of processors.

Commit: `x86: enumerate CPUs and boot APs into idle loops`

## Step 6: per-CPU scheduler and affinity

Objective: make runqueues per CPU and let `wake_up_process()` choose a target
CPU.  The APIC timer can remain the next step; the BSP timer may continue to
drive ticks while APs run runnable tasks after an IPI.

Changes:

- Replace the global runqueue in `src/kernel/sched.c` with one runqueue per
  `cpu_info`.
- Add a reschedule IPI and a simple idle-balance path.
- Add `cpu` and `cpus_allowed` to `struct task_struct`.
- Update `sys_sched_getaffinity`/`sys_sched_setaffinity` to validate against
  the online CPU map.
- Add TLB shootdown when `switch_to()` changes `mm->pd` on another CPU.

Comments required: explain task migration invariants and why an IPI is needed
when setting a remote CPU's `need_resched`.

Correctness checks:

- `make quick-run SMP=4` runs independent shell jobs on more than one CPU.
- `sched_getaffinity` returns the full online mask and
  `sched_setaffinity` rejects offline CPUs.
- Add `tests/shell/smp.sched.test` with `REQUIRES-SMP`:
  - run several CPU-heavy shell/Python loops in parallel,
  - check `sched_getaffinity` output,
  - fork/exec in a loop under `SMP=4` without memory corruption.
- Existing one-CPU tests still pass.

Commit: `sched: per-CPU runqueues, IPI wakeups, and real affinity`

## Step 7: APIC timer, IO-APIC routing, and SMP stress

Objective: give each CPU a local tick and route device IRQs through the
IO-APIC while retaining the PIC path as fallback.

Changes:

- Complete local APIC timer calibration and use it as the per-CPU scheduler
  tick.
- Program IO-APIC redirection entries for known IRQs.
- Add IPIs for reschedule, TLB shootdown, and stop/panic.
- Keep `irq_save()`/`irq_restore()` as local interrupt control; shared data
  continues to use spinlocks.

Comments required: document the PIC-to-IO-APIC transition and why IRQ
affinity/priority policy is intentionally minimal.

Correctness checks:

- `make quick-run SMP=4` reaches the shell with both APIC timer and IO-APIC
  active.
- Run the full shell suite with `--smp 4`; failures are fixed before
  proceeding.
- Run a TLB-heavy fork/exec loop with `--smp 4` for several minutes.
- Run `tests/vga` with `--smp 1` and spot-check with `--smp 4`.

Commit: `x86: APIC timer, IO-APIC routing, and IPI plumbing`

## Step 8: clone, thread groups, and minimal futex

Objective: implement shared-address-space threads well enough for a small
pthread program.

Changes:

- Refactor `do_fork()` in `src/kernel/fork.c` into
  `do_clone(tr, clone_flags, child_stack, parent_tidptr, child_tidptr, tls)`.
- Implement `sys_clone` with `CLONE_VM`, `CLONE_FILES`, `CLONE_FS`,
  `CLONE_SIGHAND`, `CLONE_THREAD`, `CLONE_PARENT_SETTID`,
  `CLONE_CHILD_SETTID`, `CLONE_CHILD_CLEARTID`, and `CLONE_SETTLS`.
- Add TGID/TID handling in `struct task_struct`, `sys_getpid`, and
  `sys_gettid`.
- Implement non-leader thread exit and thread-group-aware wait.
- Publish `clear_child_tid` before page-table teardown; `free_page_tables()`
  would otherwise unmap the userspace TID word and fault the exit path.
- Implement `SYS_exit_group` by signalling live group members, waiting for
  them to reach `TASK_ZOMBIE` and be parked by `_switch_to()`, and letting
  the leader's `do_exit()` reparent those zombies to init rather than reaping
  a task that is still switching off its kernel stack.
- Implement a real `SYS_mprotect` so musl can turn the thread-stack guard
  page from `PROT_NONE` back to readable/writable while setting up TLS.
- Keep `SYS_set_tid_address`, but actually retain the clear-child-tid address
  in the task so `CLONE_CHILD_CLEARTID` can be honored on thread exit.
- Implement classic `SYS_futex` operations needed by musl i386 pthreads:
  `FUTEX_WAIT`, `FUTEX_WAKE`, and `FUTEX_REQUEUE`; `FUTEX_PRIVATE` is
  accepted as a hint but is not used as an address namespace in this first
  implementation.  `FUTEX_CMP_REQUEUE`/`FUTEX_WAKE_OP` are optional for the
  basic create/join/mutex test and are documented as unsupported until a
  condition-variable stress test requires them.
- Preserve `sys_fork`/`sys_vfork` as wrappers.

Comments required: document thread-group ownership and which resources are
shared versus copied for each `CLONE_*` flag.

### Thread-group ownership rules

- `pid` remains the kernel thread id (TID).  `sys_gettid()` continues to
  return `current->pid`.
- Add `tgid` to `struct task_struct`.  For a process leader `tgid == pid`.
  For a `CLONE_THREAD` child `tgid = current->tgid`.
- `sys_getpid()` returns `current->tgid`, so all threads in one process share
  the same result from `getpid()`.
- `CLONE_VM`: the child stores the same `mm_struct *` as the parent and
  increments an `mm_users` refcount.  `do_fork`/`do_clone` only calls
  `copy_mm()` when `CLONE_VM` is clear.  A non-leader `do_exit()` drops its
  `mm` reference but must not run `exit_mmap()`/`free_page_tables()`.
- `CLONE_FILES`: the child stores the same `files_struct *` as the parent;
  `count` is incremented.  Non-leader thread exit must not call `exit_files()`.
- `CLONE_FS`: the child stores the same `fs_struct *` as the parent;
  `count` is incremented.  Non-leader thread exit must not call `exit_fs()`.
- A thread-group leader exiting normally performs full process teardown only
  after every other group member has committed to exit; `sys_exit_group()`
  enforces that wait, while a single thread's `sys_exit()` must not tear down
  the shared mm/files/fs.

Correctness checks:

- Add `app/test/musl/pthread/main.c`, built against musl, that creates several
  threads, shares a global counter under `pthread_mutex_lock`, and joins them.
- Verify `getpid()` is shared and `gettid()` is unique.
- Run the threaded workload with `--smp 8`, plus `--smp 4` and `--smp 1`
  smoke checks for the underlying clone/futex path.
- A non-leader thread exit does not terminate the process.
- A detached/non-leader child is waitable without corrupting process exit.
- Existing fork/vfork tests still pass.

Commit: `clone: implement shared-mm threads, thread groups, and minimal futex`

## Step 9: SMP/thread regression tests

Objective: make the new behavior continuously exercised by the normal test
suite.

Changes:

- Add `tests/shell/smp.boot.test`, `tests/shell/smp.sched.test`,
  `tests/shell/affinity.test`, and `tests/shell/pthread.test`.
- Add pthread stress tests covering repeated create/join rounds, detached
  threads, nested pthread creation, and a wide 48-thread pool.
- Teach the lit shell runner to skip `REQUIRES-SMP` tests when `--smp` is not
  supplied and to run them when `--smp N` is supplied with `N > 1`.
- Run both `tests/shell` and `tests/vga` under `--smp 1` and `--smp 4` in CI
  style.
- The pthread test is `REQUIRES-SMP: 8` when it exercises the all-CPU
  scheduler, with a smaller `--smp 4` and `--smp 1` smoke check in the same
  file.

Correctness checks:

- `python3 tests/lit_shell_test.py tests/shell --smp 1` passes.
- `python3 tests/lit_shell_test.py tests/shell --smp 4` passes all applicable
  tests and skips none that are supposed to run.
- The new pthread/affinity tests pass on QEMU `-smp 4`.

Commit: `tests: add SMP, affinity, and pthread regression coverage`

## Risks and mitigations

- **PIC-only assumptions are deeply embedded.** Keep the PIC path for UP
  fallback and land AP boot before IO-APIC routing.
- **Interrupt masking is not mutual exclusion.** Convert the allocator, task
  list, wait queue, timer, VMA, and file-map locks before booting APs.
- **`current` via `%esp` is assembly-visible.** Switch to `%fs` while still UP
  and keep the change independently testable.
- **The global file-map cache is the newest unlocked list.** Lock it before
  AP boot; otherwise concurrent faults, `msync`, and truncate can corrupt it.
- **`vm_file` makes `f_count` cross-task.** Fix `release_file()` while doing
  the mmap locking work, before shared threads are possible.
- **QEMU provides MP/MADT tables but BIOS details vary.** If no table is
  found, stay UP instead of guessing.
- **Thread semantics touch many syscalls.** Land `clone` without
  `CLONE_THREAD` only if a simpler intermediate test is needed, then add
  thread groups in the same step with explicit pid/tid checks.

## Out of scope for this plan

- NUMA, CPU hotplug, power management, and heterogeneous topology.
- Full ACPI namespace parsing and arbitrary IO-APIC routing policies.
- `clone3` and futex variants beyond the classic operations needed by the
  pthread test.
- A production scheduler; the existing counter/priority policy is retained,
  made per-CPU and SMP-safe.
- A per-CPU or NUMA-aware page allocator; a single locked buddy allocator is
  sufficient.
