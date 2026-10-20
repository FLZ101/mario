# SMP and Multithreading Support Plan

## Goal

Extend Mario from the current single-CPU, single-runqueue kernel to:

- Detect and boot multiple x86 processors.
- Schedule tasks on more than one CPU.
- Expose CPU affinity through the existing `sched_setaffinity` and
  `sched_getaffinity` syscalls instead of hard-coding CPU 0.
- Implement enough of the `clone` syscall and thread-group semantics for
  userspace multithreading, including `CLONE_VM`, `CLONE_FILES`, `CLONE_FS`,
  `CLONE_SIGHAND`, and `CLONE_THREAD`.
- Keep the existing single-CPU path working unchanged when only one CPU is
  present or APIC support is unavailable.

This is intentionally split into two concerns:

1. **SMP**: make the kernel correct on multiple CPUs and actually use them.
2. **Multithreading**: let one process have multiple schedulable threads that
   share its address space and file/fs/signal context.

Multithreading is easier to make reliable after the SMP-safe synchronization
primitives and per-CPU infrastructure exist, so the milestones below put the
shared SMP groundwork first.

## Current State and Blockers

The following properties are incompatible with more than one CPU or with
POSIX-style shared-address-space threads.

### One CPU, one runqueue

- `src/kernel/sched.c` has a single global `runqueue_head`.
- `schedule()` scans all tasks and one runqueue, and always assumes the
  scheduler is running on the only CPU.
- `wake_up_process()` inserts directly into that single runqueue without
  selecting a target CPU.
- `src/kernel/sys.c:183` and `src/kernel/sys.c:212` reject every affinity
  mask except CPU 0.

### `current` is derived from the stack pointer

- `src/include/task.h:106` computes `get_current()` by masking `%esp` with
  `~8191`.
- `src/kernel/entry.S` uses `GET_CURRENT` based on `%esp`.
- This works only because each task has a separate 8 KiB kernel stack and only
  one CPU uses it at a time. SMP needs a per-CPU `current` pointer, normally
  through `%fs` or another per-CPU segment.

### One GDT/TSS/TLS table

- `src/include/tss.h` defines a single global `struct tss_struct init_tss`.
- `src/kernel/tss.c` initializes only that one TSS.
- `src/kernel/start.S` loads one GDT and one TSS at boot.
- `src/kernel/tls.c` writes the shared TLS GDT slots. That is not safe when two
  CPUs switch tasks concurrently; each CPU needs its own GDT/TSS/TLS slots.

### Legacy PIC-only interrupt model

- `src/kernel/irq.c` initializes only the 8259A master/slave PICs.
- `src/kernel/entry.S` installs IRQ gates for vectors `0x20` through `0x2f`.
- There is no local APIC, IO-APIC, IPI, APIC timer, MADT, or Intel MP table
  code.
- `cli`/`sti` only affect the current CPU, so the many places using `irq_save`
  to protect shared data are not SMP-safe.

### Locking primitives are UP-only

- `src/include/lib/spinlock.h` uses `xchgl` without a `lock` prefix or
  release/acquire ordering.
- `src/include/lib/atomic.h` uses unlocked `addl`/`incl`/`decl`.
- `src/include/lib/bitops.h` uses unlocked `bts`/`btr`/`btc`.
- These happen to be correct only when interrupts are disabled on one CPU.

### Allocator, VMA, and kernel data structures use interrupt masking

- `src/mm/page_alloc.c` protects the buddy free lists with `irq_save()`.
- `src/mm/kmalloc.c` also relies on interrupt masking.
- `src/mm/mm.c` manipulates `struct vm_area_struct` lists without an mm lock.
- `src/mm/fault.c` reads and updates current task/page state without a cross-CPU
  VMA lock.
- Task-list macros in `src/include/sched.h` (`SET_LINKS`, `REMOVE_LINKS`) use
  `irq_save()`, which only protects against local interrupts.

### No clone/CLONE_THREAD

- `src/include/syscall.h` already defines `SYS_clone` and `SYS_clone3`, but
  `src/kernel/entry.S:493` maps `SYS_clone` to `sys_not_exist`.
- `src/kernel/fork.c` only implements `sys_fork` and `sys_vfork`; `do_fork()`
  always copies `mm`, `fs`, and `files`.
- There is no `sys_clone`, no `CLONE_*` flags, no thread-group/TGID model, and
  no `sys_futex`.
- `src/kernel/entry.S:613` maps `SYS_futex` to `sys_not_exist`, which most
  pthread implementations need for proper mutexes, condition variables, and
  synchronization.

### CPU discovery and AP startup absent

- `src/kernel/mario.c` boots only the BSP and drops into `cpu_idle()`.
- `src/include/multiboot.h` contains memory/VBE/module information but no SMP
  CPU table.
- The kernel does not parse ACPI/MADT or the Intel MP table and does not issue
  INIT-SIPI for application processors.

## Proposed Architecture

### Per-CPU state

Add a `struct cpu_info` containing at least:

```c
int id;
int online;
struct task_struct *current_task;
struct runqueue rq;
struct tss_struct tss;
struct desc_struct *gdt;      /* or per-CPU GDT area */
struct task_struct *idle;
uint64_t idle_ticks;
```

Use `%fs` as the current-CPU base on i386:

- Reserve a GDT selector for the per-CPU segment.
- On BSP boot and AP entry, set the segment base to that CPU's `cpu_info`.
- Replace `get_current()` and `GET_CURRENT` with reads through this selector,
  for example `current` at a fixed offset inside `cpu_info`.
- Keep the old ESP-masking path only temporarily behind a compile/runtime
  fallback for the single-CPU bring-up step.

This also removes the implicit requirement that a task can be identified by
its stack alignment, which makes thread/CPU debugging simpler.

### CPU discovery

Prefer ACPI MADT parsing:

- Find the RSDP and RSDT/XSDT in conventional memory or from the Multiboot
  information when the boot loader exposes it.
- Parse MADT local-APIC entries to enumerate logical CPU IDs.
- Use CPUID to validate APIC support before trying to start APs.
- If MADT is unavailable, add an Intel MP floating-pointer/table parser as a
  fallback for QEMU and older machines.

AP startup:

- Allocate a low-memory real-mode/protected-mode trampoline page below 1 MiB.
- Prepare an AP boot stack and a temporary GDT/IDT page table.
- Send INIT-SIPI-SIPI with the startup address pointing at the trampoline.
- In the trampoline, enable protected mode and paging, switch to the kernel
  page directory, load the CPU-local GDT/TSS/IDT, and jump to
  `smp_ap_entry(cpu_id)`.
- `smp_ap_entry()` marks the CPU online, initializes its per-CPU pointer and
  idle task, starts its APIC timer, and enters the per-CPU idle loop.

Do not start APs unless both CPUID and a usable APIC/MADT/MP table are present.
The current PIC-only path remains the fallback for `-smp 1` or hardware without
APIC.

### Interrupt and timer model

- Keep the 8259 PIC path for compatibility and uniprocessor fallback.
- Add local APIC base access, EOI, timer calibration, and IPI sending in
  `src/kernel/apic.c`.
- Add IO-APIC redirection-table programming for real IRQ routing.
- Use the local APIC timer as the per-CPU scheduler tick once available.
- Add IPIs for:
  - reschedule requests,
  - TLB shootdown when switching address spaces,
  - panic/stop requests,
  - cross-CPU function calls for debugging if needed.
- Make `irq_save()`/`irq_restore()` remain local-interrupt operations. Shared
  state must additionally take a spinlock.

The initial SMP milestone can boot APs and let them run their idle loops while
the BSP still handles all device IRQs. This separates CPU bring-up from the
more invasive IO-APIC interrupt routing work.

### Scheduler

Replace the single runqueue with per-CPU runqueues plus one global task list:

- `src/include/cpumask.h`: CPU masks used by affinity and load balancing.
- `src/include/smp.h`: CPU count, online map, `smp_processor_id()`,
  `for_each_online_cpu()`.
- Per-CPU runqueue protected by a real spinlock.
- `schedule()` only touches its local runqueue.
- `wake_up_process()` selects a CPU using the task's affinity mask and a simple
  wake balancing rule.
- Add periodic load balancing, first based on runqueue length and later on
  counter/priority weight.
- Keep the existing counter/priority algorithm initially; the scheduler change
  should be about runqueue placement and SMP safety, not a scheduling-policy
  rewrite.
- Store `cpu` and `cpus_allowed` in `struct task_struct`.
- Update `sys_sched_setaffinity`/`sys_sched_getaffinity` to validate against
  the online CPU map and to wake or migrate the task as needed.

### Memory management

`mm_struct` and VMA changes:

- Add an `mm_users`/`mm_count` reference model and an `mm_lock`.
- A normal `fork` increments/creates an `mm` as today.
- `CLONE_VM` shares the same `mm` instead of calling `copy_mm()`.
- VMA list operations (`insert_vm_struct`, `unmap_fixup`, `dup_mmap`,
  `exit_mmap`, `find_vma`) take the mm lock when modifying shared state.
- Page-fault and COW paths use the same mm lock or a smaller fault mutex;
  no cross-CPU VMA traversal may be lock-free.

Allocator changes:

- Add a global buddy/`kmalloc` spinlock to `src/mm/page_alloc.c` and
  `src/mm/kmalloc.c`.
- Replace `irq_save()` as the only protection for free lists with
  `spin_lock_irqsave()`.
- Ensure buddy bitmap updates use locked bit operations.
- The page reference count must continue to use atomic operations, now with
  `lock` prefixes.

### File, filesystem, and signal sharing

- `CLONE_FILES`: share `files` instead of `copy_files()`.
- `CLONE_FS`: share `fs` instead of `copy_fs()`.
- `CLONE_SIGHAND`: share signal handlers; block mask and pending signals remain
  per thread where required.
- Make `struct file` reference-counted for shared file tables.
- Protect the global file/inode/buffer/superblock tables with the already
  introduced real spinlocks; their current locks become SMP-safe once the
  spinlock implementation is fixed.

### Thread model

Implement `sys_clone` with the i386 Linux ABI:

```c
int sys_clone(unsigned long flags, void *child_stack,
              int *parent_tid, int *child_tid, unsigned long tls);
```

Refactor `do_fork()` in `src/kernel/fork.c` to take `clone_flags`:

- `CLONE_VM`: share `mm`, do not copy page tables, and do not free them on
  thread exit until the last mm user exits.
- `CLONE_FILES`/`CLONE_FS`/`CLONE_SIGHAND`: share the corresponding structure.
- `CLONE_THREAD`: put the new task in the caller's thread group; assign a
  distinct TID while keeping the visible `getpid()` as the TGID.
- `CLONE_PARENT_SETTID`/`CLONE_CHILD_SETTID`/`CLONE_CHILD_CLEARTID`: write TID
  values and clear at exit.
- `CLONE_SETTLS`: install the supplied TLS descriptor for the new thread.
- If `child_stack` is nonzero, make `copy_thread()` set `thread.esp` from it
  instead of reusing the parent frame.
- Preserve `sys_fork` and `sys_vfork` as wrappers over the same `do_fork()`.

Thread exit/wait:

- `do_exit()` must handle non-leader thread exit without treating it as a full
  process exit.
- `wait*` semantics should initially expose children of the process/thread
  group correctly and keep the existing parent-child relationship.
- `sys_getpid()` returns TGID while `sys_gettid()` returns TID.
- Store the `set_tid_address` pointer in `struct task_struct` and clear it on
  exit.

Minimal futex:

- Implement the classic `SYS_futex` operations needed by pthreads first:
  `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, and
  `FUTEX_REQUEUE` as needed.
- Keep `clone3` and newer futex variants out of scope until the basic pthread
  test passes.

## File-Level Change List

New files:

- `src/include/smp.h`
- `src/include/percpu.h`
- `src/include/cpumask.h`
- `src/kernel/smp.c`
- `src/kernel/smp_boot.S` or a low-memory trampoline under `src/kernel/`
- `src/kernel/apic.c`
- `src/kernel/ioapic.c`
- `src/kernel/madt.c` or `src/kernel/mp.c`
- `src/include/acpi.h` if parsing RSDT/XSDT

Modified files:

- `src/kernel/mario.c`: call `smp_init()`/`apic_init()` in the boot sequence.
- `src/kernel/start.S`: add per-CPU GDT/TSS initialization hooks and AP entry
  support.
- `src/kernel/entry.S`: replace `GET_CURRENT` with `%fs`-based current, add
  IPI/APIC interrupt gates, update the `SYS_clone` table entry.
- `src/kernel/sched.c`: per-CPU runqueues, CPU selection, load balancing,
  reschedule IPI.
- `src/include/sched.h`: real task-list locks and clone-aware scheduling
  helpers.
- `src/include/task.h`: add TID/TGID, thread-group, affinity, CPU, exit
  pointers, shared-resource pointers, and mm user counts.
- `src/kernel/fork.c`: `do_fork(clone_flags)` and `sys_clone`.
- `src/kernel/exit.c`: thread exit, thread-group exit, TID clearing.
- `src/kernel/sys.c`: `getpid`/`gettid`, `set_tid_address`, futex, real CPU
  affinity.
- `src/kernel/irq.c`: keep PIC, integrate APIC/IO-APIC interrupt routing.
- `src/kernel/timer.c`: per-CPU APIC timer path.
- `src/kernel/tss.c` and `src/include/tss.h`: per-CPU TSS.
- `src/kernel/tls.c`: use the current CPU's GDT/TLS slots.
- `src/include/lib/spinlock.h`: `lock xchg`/`lock cmpxchg`, barriers, and
  `spin_lock_irqsave()`.
- `src/include/lib/atomic.h` and `src/include/lib/bitops.h`: add `lock`
  prefixes/memory clobbers to SMP operations.
- `src/mm/page_alloc.c`, `src/mm/kmalloc.c`, `src/mm/mm.c`,
  `src/mm/fault.c`: replace interrupt-mask-only protection with spinlocks.
- `src/include/mm/mm.h`: add mm lock/reference fields.
- `src/fs/file.c`, `src/fs/pipe.c`, and other global table users: use the
  fixed spinlock rather than relying on local interrupt masking.

## Milestones

### 1. SMP-safe primitives only, still booting on one CPU

- Fix `spinlock.h`, `atomic.h`, and `bitops.h` with locked operations and
  barriers.
- Add `spin_lock_irqsave()`/`spin_unlock_irqrestore()`.
- Convert page allocator, `kmalloc`, task-list macros, and global filesystem
  tables from `irq_save()` to real locks.
- Add the per-CPU data abstraction and switch `current` to a `%fs`-based
  pointer while still booting only the BSP.

Validation:

- Existing shell and VGA lit tests still pass with `-smp 1`.
- `make` and the normal `qemu/Makefile` targets remain unchanged.
- Add a short stress test that forks, allocates, and reads/writes files on one
  CPU to catch lock-conversion regressions.

### 2. CPU enumeration and AP boot

- Parse CPUID, APIC, MADT/MP table.
- Add AP trampoline and `smp_ap_entry()`.
- Boot APs into their own idle loops with per-CPU GDT/TSS and current pointer.
- Add `cpu_info`, online map, and `nproc`/`/proc/cpuinfo`-style debug output.

Validation:

- Boot QEMU with `-smp 2` and `-smp 4`.
- Kernel logs one line per online CPU and remains stable at the shell.
- The BSP still handles interrupts and userspace works while APs idle.

### 3. SMP scheduler and interrupts

- Add per-CPU runqueues.
- Implement reschedule IPIs and basic load balancing.
- Add APIC timer as the local tick when available.
- Wire affinity syscalls to the online CPU map.
- Add TLB shootdown when switching page directories on different CPUs.

Validation:

- Run several independent shell/Python workloads simultaneously.
- Use `-smp 4` and observe tasks running on more than one CPU.
- `sched_getaffinity` returns the full online mask.
- `sched_setaffinity` rejects offline CPUs and migrates a runnable task.
- A TLB-heavy fork/exec loop under `-smp 4` does not crash or corrupt memory.

### 4. Shared-MM multithreading

- Refactor `do_fork()` and add `sys_clone`.
- Implement `CLONE_VM`, `CLONE_FILES`, `CLONE_FS`, `CLONE_SIGHAND`,
  `CLONE_THREAD`, TID/TGID, and child-stack handling.
- Implement `SYS_set_tid_address` and minimal `SYS_futex`.
- Fix `exit`/`wait` semantics for thread groups.

Validation:

- Build and run a small pthread program that creates several threads, shares a
  global counter, mutates it under a mutex, and joins them.
- Verify `getpid()` is shared and `gettid()` is unique per thread.
- Run the same threaded workload with `-smp 2` and `-smp 4`.
- Run a thread that exits without causing the process to terminate.
- Run a process where only the thread-group leader waits for detached/non-leader
  threads.

### 5. Hardening and regression coverage

- Add lit tests:
  - `tests/shell/smp.test`: `nproc`, `cat /proc/cpuinfo` if exposed, parallel
    shell jobs.
  - `tests/shell/pthread.test` or a C test binary for thread creation, mutex,
    condition variable, and join.
  - `tests/shell/affinity.test`: set/get affinity and verify error handling.
- Run all existing `tests/shell` and `tests/vga` suites with `-smp 1` and
  `-smp 4`.
- Stress test fork/exec, pipe, VMA allocation/free, COW, and thread exit in
  parallel.

## QEMU/Test Integration

Add SMP variants to `qemu/Makefile`:

```make
smp2-run: tmp/kernel.exe tmp/rdx
	qemu-system-i386 -smp 2 -m 256M -kernel tmp/kernel.exe \
		-initrd tmp/rdx -display none -serial stdio \
		-no-reboot -no-shutdown

smp4-run: tmp/kernel.exe tmp/rdx
	qemu-system-i386 -smp 4 -m 256M -kernel tmp/kernel.exe \
		-initrd tmp/rdx -display none -serial stdio \
		-no-reboot -no-shutdown
```

The existing `tests/lit_shell_test.py` QEMU command can gain a `--smp N`
option. Until SMP support lands, keep the default at one CPU so current tests
continue to pass.

## Risks and Mitigations

- **PIC-only assumption is deeply embedded.** Do not remove the PIC path;
  keep it as the UP fallback. Land AP boot and IO-APIC routing as separate
  milestones.
- **Locks currently imply local interrupt disable.** Converting all callers
  atomically is large. Start with one real spinlock primitive and convert the
  allocator, task list, and scheduler first.
- **`current` via ESP is used in assembly.** Keep a temporary single-CPU
  fallback and switch `%fs` setup early in both BSP and AP boot.
- **Shared GDT/TLS is racy.** Move GDT/TSS/TLS into per-CPU data before
  enabling AP scheduling; otherwise TLS-using threads will corrupt each
  other's descriptors.
- **MADT/MP-table parsing adds BIOS dependency.** QEMU reliably provides
  ACPI/MADT; make the MP table a secondary parser. If no table is found,
  intentionally stay UP rather than guessing.
- **Thread semantics affect many syscalls.** `getpid`, `gettid`, `exit`,
  `wait`, signals, and futex all need coordinated changes. Land `clone`
  without `CLONE_THREAD` first, then add thread groups as a separate step.
- **i386 does not have swapgs/`gsbase`.** Use a per-CPU GDT selector for
  `%fs` and reload it on CPU entry/context switch.

## Out of Scope for the First Implementation

- NUMA, CPU hotplug, power management, and heterogeneous topology.
- Full ACPI namespace parsing and arbitrary IO-APIC routing policies.
- `clone3` and the newer `futex_waitv` family unless required by a specific
  test or libc.
- Replacing the current priority/counter scheduler with a production scheduler;
  the first change is only to make it SMP-safe and runqueue-aware.
- Per-CPU heap or NUMA-aware page allocator; one global buddy allocator with a
  real lock is sufficient for the initial implementation.

