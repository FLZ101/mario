---
name: kernel-code-check
description: Review Mario kernel C changes for user-memory copies or blocking calls inside spinlock/irqsave regions. Use for kernel concurrency reviews and page-fault deadlock prevention.
---

# Kernel Code Check

Use this skill when reviewing kernel code under `src/` for locking hazards,
especially the failure mode where a task sleeps while holding a spinlock and
another task spins forever on that lock.

## What to find

Find paths that can sleep while a non-sleepable lock is held or while local
interrupts are disabled. The highest-risk pattern is user-memory access inside
a spinlock-protected region.

The concrete Mario rules are:

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

   The scanner reports only likely user-memory helpers and `u=1` ring-buffer
   calls inside spinlock regions. Treat each hit as requiring manual review;
   some hits may be safe because of control flow the scanner does not model.

2. For the changed functions, inspect every `spin_lock_irqsave`,
   `spin_lock`, or `ACQUIRE_LOCK` region and confirm it contains no blocking
   call. Search for nested wait helpers such as `sleep_on` and `schedule`.

3. Trace all user-memory helpers found outside the scanner’s mechanical pass
   to verify no spinlock is held at that point.

4. For data-transfer paths, prefer staging user data in a `kmalloc` kernel
   buffer and touching user memory only before `spin_lock_irqsave` or after
   `spin_unlock_irqrestore`.

5. Rebuild with `make kernel` and run the relevant kernel or GUI fixtures.

## Manual searches

```sh
rg -n "spin_lock_irqsave|spin_lock\(|ACQUIRE_LOCK" src
rg -n "memcpy_(tofs|fromfs|fs)|(get|put)_fs_long|verify_area" src
rg -n "\bsleep_on\b|\bschedule\b|wait_queue" src
rg -n "ring_buffer_(read|write)\([^;]*,\s*1\s*\)" src
```

## Fix pattern

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
unless the review finds an actual sleep-under-spinlock hazard.
