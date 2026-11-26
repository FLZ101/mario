---
name: kernel-code-check
description: Review kernel C changes for user-memory copies, blocking calls, and configured lock-order inversions inside spinlock/irqsave regions. Use for kernel concurrency reviews and deadlock prevention.
---

# Kernel Code Check

Use this skill when reviewing kernel C code for locking hazards: sleeping or
copying user memory while a spinlock is held, and nesting independently
configured locks in an unsafe order.

The scanner is generic. Codebase-specific lock names and forbidden pairs live
in a JSON rules file, not in the scanner source.

## Rule file

The default rules for this project are in:

```text
.codex/skills/kernel-code-check/config/default.json
```

Override it for another codebase:

```sh
python3 scripts/check_kernel_locks.py --config path/to/rules.json path/to/src
```

The JSON file defines:

- `generic_lock_pattern` and `generic_unlock_pattern` for any spinlock region.
- `unsafe_checks` for operations that are unsafe inside a spinlock region.
- `locks` with named `lock_pattern` and `unlock_pattern` regexes.
- `forbidden_pairs` that are never allowed to nest in either direction.

The included default config declares `tasklist_lock` and `mm_lock` as
mutually non-nestable.

## What to find

Find paths that can sleep while a non-sleepable lock is held or while local
interrupts are disabled. The highest-risk pattern is user-memory access inside
a spinlock-protected region.

The default Mario rules cover:

- `spin_lock_irqsave()` runs `cli` before acquiring the lock. If the holder
  then sleeps, it may never receive the wakeup IPI and may never resume.
- `memcpy_tofs`, `memcpy_fromfs`, `memcpy_fs`, `get_fs_long`, and `put_fs_long`
  touch user memory and can page-fault.
- `ring_buffer_read(..., 1)` and `ring_buffer_write(..., 1)` copy user memory.
  The `0` variants copy kernel memory.
- `verify_area()` checks mappings but does not prevent later demand-paging
  faults.
- `sleep_on()` releases only the lock passed to it, not an outer spinlock.

## Workflow

1. Run the scanner for high-confidence mechanical hits:

   ```sh
   python3 .codex/skills/kernel-code-check/scripts/check_kernel_locks.py src
   ```

   The scanner reports configured unsafe operations and configured lock-order
   inversions inside spinlock regions. Treat each hit as requiring manual
   review; some hits may be safe because of control flow the scanner does not
   model.

2. For the changed functions, inspect every `spin_lock_irqsave`,
   `spin_lock`, or `ACQUIRE_LOCK` region and confirm it contains no blocking
   call. Search for nested wait helpers such as `sleep_on` and `schedule`.

3. Trace all user-memory helpers found outside the scanner's mechanical pass
   to verify no spinlock is held at that point.

4. For data-transfer paths, prefer staging user data in a kernel buffer and
   touching user memory only before `spin_lock_irqsave` or after
   `spin_unlock_irqrestore`.

5. Rebuild the kernel and run the relevant fixtures.

## Manual searches

```sh
rg -n "spin_lock_irqsave|spin_lock\(|ACQUIRE_LOCK" src
rg -n "memcpy_(tofs|fromfs|fs)|(get|put)_fs_long|verify_area" src
rg -n "\bsleep_on\b|\bschedule\b|wait_queue" src
rg -n "ring_buffer_(read|write)\([^;]*,\s*1\s*\)" src
rg -n "tasklist_lock|mm_lock\(|mm_unlock\(" src
```

## Fix pattern for configured lock inversions

For a configured invalid pair such as `tasklist_lock -> mm_lock`, avoid the
second lock. Prefer atomic counters that are maintained under the second lock
and read without it:

```c
spin_lock_irqsave(&tasklist_lock, flags);
task = proc_find_task_locked(pid);
if (task && task->mm) {
    vm_pages = atomic_read(&task->mm->vm_pages);
    rss_pages = atomic_read(&task->mm->resident_pages);
}
spin_unlock_irqrestore(&tasklist_lock, flags);
```

## Fix pattern for user-copy under spinlocks

For AF_UNIX/pipe-style transfers, keep the ring-buffer update `u=0` under the
lock and stage user bytes around the lock.

```c
/* Write path: copy from user before taking the lock. */
staging = kmalloc(stage);
if (!staging)
    return -ENOMEM;
memcpy_fromfs(staging, user_buf, stage);

spin_lock_irqsave(&pair->lock, flags);
n = ring_buffer_write(&peer->receive, staging, stage, 0);
spin_unlock_irqrestore(&pair->lock, flags);
kfree(staging);

/* Read path: copy into staging under the lock, then out after unlock. */
spin_lock_irqsave(&pair->lock, flags);
n = ring_buffer_read(&socket->receive, staging, stage, 0);
spin_unlock_irqrestore(&pair->lock, flags);
memcpy_tofs(user_buf, staging, n);
kfree(staging);
```

## Boundaries

Do not flag `u=0` kernel copies. Do not flag user-memory access that provably
occurs outside all spinlock regions. Do not rewrite unrelated locking code
unless the review finds an actual sleep-under-spinlock or lock-order hazard.
