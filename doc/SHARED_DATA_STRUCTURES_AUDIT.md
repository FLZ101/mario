# Shared Kernel Data Structures Audit

Status: audit only.  The items below are the shared structures and accessors
that need SMP fixes.  They are ordered by risk; each fix should be one commit
and should be validated with `make test-all` before committing.

## 1. Global task list

**What is shared**

`init_task`-linked task list, together with the child/sibling links
(`p_cptr`, `p_ysptr`, `p_osptr`).

**Current protection**

`SET_LINKS` and `REMOVE_LINKS` in `src/include/sched.h` take
`tasklist_lock`, but `for_each_task()` does not:

```c
#define for_each_task(p) \
    for (p = &init_task; (p = p->next_task) != &init_task; )
```

**Why it is broken**

- `src/kernel/sys.c` and `src/kernel/exit.c` walk the list without
  `tasklist_lock` for `getpgid`, `setpgid`, `get/setpriority`,
  `sched_get/setaffinity`, `kill`, `kill_pg`, `kill_sl`, `is_orphaned_pgrp`,
  and session/TTY lookups.
- A concurrent fork/exit can unlink and free a task while another CPU is
  following `next_task`.

**Planned fix**

Add locked task iteration helpers, or wrap each traversal in
`spin_lock_irqsave(&tasklist_lock, flags)`.  Avoid a macro that cannot unlock
on `return`/`goto`; convert call sites explicitly.  Then lock per-task signal
state for `send_sig`.

## 2. Per-task signal and scheduling state

**What is shared**

`p->state`, `p->signal`, `p->sigaction[]`, `p->blocked`, `p->exit_code`,
`p->priority`, and `p->cpus_allowed`.

**Current protection**

None for `send_sig()`; it mutates a target task while that task may be
running on another CPU (`src/kernel/exit.c`).  Signal delivery reads the same
fields in `src/kernel/signal.c`.

**Why it is broken**

Lost signal bits, stale state transitions, and torn `sigaction` copies are
possible under SMP.

**Planned fix**

Use `task->lock` (or the scheduler lock, if it is already held by all
state-transition paths) to serialize signal pending/action updates.  Keep
`SIGKILL`/`SIGCONT` stop-state transitions under the same lock.

## 3. TTY input ring buffer

**What is shared**

`struct tty_struct.read_buf` (`struct ring_buffer`) and `termios`.

**Current protection**

None.  `tty_receive_c()` (`src/fs/char/tty.c:52`) runs from IRQ/keyboard
context, while `tty_read()` and `tty_lseek()` mutate the same ring buffer.
`set_termios()` only does local `cli(); sti();`, which does not protect
against another CPU.

**Why it is broken**

Concurrent `ring_buffer_write` and `ring_buffer_read` can corrupt head/tail
counts, and a reader can observe a torn `termios` update.

**Planned fix**

Add a spinlock to `struct tty_struct` protecting `read_buf` and `termios`.
Use IRQ-safe variants where input arrives in interrupt context.  Remove
`cli/sti` around termios and replace with the lock.

## 4. Console/VGA shared state

**What is shared**

`console_table`, `console_tty_table`, `fg_console`, `debug_console`, and the
direct `VIDEO_MEM` mapping in `src/fs/char/console.c`.

**Current protection**

`switch_fg_console()` uses only local `irq_save/irq_restore`; normal console
write and alternate-buffer updates access `VIDEO_MEM` without a common lock.

**Why it is broken**

Different CPUs writing to `VIDEO_MEM`, switching the foreground console, or
touching `con->mem` concurrently can produce corrupted display state.

**Planned fix**

Introduce a console state spinlock (IRQ-safe).  Serialize console switch,
screen copy, and foreground console reads.  Keep actual rendering short.

## 5. Syscall trace counters

**What is shared**

`syscall_trace[]` in `src/kernel/syscall_trace.c`.

**Current protection**

None.  `syscall_trace_end()` increments `calls`, `cycles`, and `max_cycles`
with plain `++`/`+=`.

**Why it is broken**

Updates can be lost across CPUs.  This is low severity for a debug facility
but can mislead performance debugging.

**Planned fix**

Make the per-syscall stat increments atomic, or use a small per-CPU counter
array and aggregate at read time.

## 6. Superblock and mount table

**What is shared**

`static struct super_block all[NR_SUPER]` in `src/fs/super_block.c`, plus
`file_systems` (`struct list_head`, `src/fs/file_system.c`).

**Current protection**

`sb_sem` exists and `mount_root()` holds it, but `sys_mount()` /
`sys_umount()` call `read_super`, `get_super`, and `do_umount_sb` without
consistently holding `sb_sem`.  `file_systems` is registered at boot but is
traversed at runtime without a lock.

**Why it is broken**

Two concurrent mounts can pick the same free `all[]` slot or observe a
half-updated superblock; unregister/register can race with lookups.

**Planned fix**

Hold `sb_sem` around the full mount/umount table mutation and lookup.
Optionally add a simple `file_systems_lock` for the list.

## 7. Bottom-half bitmap

**What is shared**

`bh_active`, `bh_mask`, and `bh_base` in `src/kernel/bh.c`.

**Current protection**

None.  `do_bottom_half()` does `bh_active &= ~mask`, which is read-modify-write.

**Why it is broken**

Concurrent bottom-half scheduling/latent handling can lose bits.

**Planned fix**

Use the existing `test_and_clear_bit` style locked bitmap operations for
`bh_active`, or protect the operation with an IRQ-safe spinlock.

## 8. `ioremap` singleton state

**What is shared**

`iomap_virt` and `iomap_size` in `src/mm/ioremap.c`.

**Current protection**

None.  `ioremap()` checks `iomap_virt` and then writes it later.

**Why it is broken**

Two CPUs can pass the `if (iomap_virt)` check and corrupt the swapper page
table / mapping bookkeeping.

**Planned fix**

Add a spinlock around the single active mapping check, page-table setup, and
state update.  This is low priority because current drivers mostly map once
at boot, but should be made SMP-safe.

## Already protected

These are included so later review does not regress them:

- File descriptor table: `file_lock`
- Inode cache: `inode_lock`
- Buffer cache: `buffer_lock` + per-buffer `b_sem`
- Page allocator: `page_alloc_lock`
- Kernel heap: `kmalloc_lock`
- Task-list insertion/removal: `tasklist_lock`
- Scheduler runqueues/state: `scheduler_lock` + per-runqueue locks
- `last_pid` allocation: `last_pid_lock`
- Printk/log buffer: `logbuf_lock`
- Wait and futex queues: wait/futex locks

## Validation for each commit

```bash
make kernel -j$(nproc)
make test-all
```

Any fix that changes kernel data-structure ownership must be accompanied by a
short comment explaining the lock/order chosen.
