# procfs SMP deadlock

## Summary

`make run-gtk SMP=8`, followed by a terminal plus fullscreen `btop`, freezes
after several minutes. The screen stops updating, and all keys (including the
F1-F8 console-switch keys) stop working.

The freeze is a kernel deadlock caused by the procfs memory-reporting change:
`/proc/<pid>/stat` and `/proc/<pid>/status` acquired `mm_lock` while already
holding `tasklist_lock`.

## Root cause

The new procfs code used this lock order:

```c
spin_lock_irqsave(&tasklist_lock, flags);
...
mm_lock(task->mm, &mm_flags);
get_vm_size(task->mm);
...
```

That creates:

```text
tasklist_lock -> mm_lock
```

On SMP this conflicts with the existing kernel ordering. `schedule()` takes
`tasklist_lock` first:

```c
irq_save();
spin_lock(&tasklist_lock);
spin_lock(&scheduler_lock);
```

Process creation, exit, and signal paths also use `tasklist_lock`. The VMA and
address-space paths use `mm_lock`. Once procfs introduced the opposite
dependency, the system could enter an AB-BA cycle:

- CPU A: btop's procfs reader holds `tasklist_lock` and waits for `mm_lock`.
- CPU B: a process holds `mm_lock` during address-space work and then waits
  for `tasklist_lock` on its way into the scheduler.

Both CPUs spin with interrupts disabled, so timer, keyboard, and
VT-switch/TTY interrupts are no longer serviced. This matches the symptom: the
display freezes and F1-F8 stop working.

btop makes the window easy to hit because its collector thread repeatedly
reads `/proc/<pid>/stat` and `/proc/<pid>/status` for every process while the
rest of the system is forking, mapping, and exiting.

## Fix

The procfs read paths no longer take `mm_lock` under `tasklist_lock`.

- `src/include/mm/mm.h` adds `atomic_t vm_pages`.
- `src/mm/mm.c` maintains `vm_pages` when VMAs are inserted, removed, and
  torn down.
- `src/fs/proc.c` reads `vm_pages` and `resident_pages` atomically instead of
  calling `get_vm_size()` under `mm_lock`.

`VmSize` and `VmRSS` are still reported correctly, but procfs only reads two
atomic counters while holding `tasklist_lock`. The
`tasklist_lock -> mm_lock` dependency is gone.

## Validation

- `python3 .codex/skills/kernel-code-check/scripts/check_kernel_locks.py src`
- `make kernel`
- `tests/shell/proc.test` with `SMP=8`
- `tests/tty/btop.test` with `SMP=8`
- `tests/shell/fltk-system-monitor-model.test` with `SMP=8`
- Additional 25-second btop run with `SMP=8`
