---
name: code-check
description: Review and scan kernel C (`src/`), userspace GUI/IPC code (`app/`), and guest test fixtures (`tests/`) for spinlock/user-copy/lock-order hazards, AF_UNIX surface-upload socket invariants, compositor stdio leaks, and GUI test window-focus races or fixture isolation bugs.
---

# Mario Code Check

Use this skill when reviewing or modifying kernel code under `src/`, userspace
GUI/compositor clients under `app/`, or guest shell/TTY/GUI fixtures under
`tests/`.

## Scanner entrypoints

Run the unified checker across `src`, `app`, and `tests` (default):

```sh
python3 .codex/skills/code-check/scripts/check_code.py
```

Or target specific directories or files:

```sh
python3 .codex/skills/code-check/scripts/check_code.py src
python3 .codex/skills/code-check/scripts/check_code.py app tests
```

The standalone kernel lock scanner `scripts/check_kernel_locks.py` remains
available for custom JSON rule files (`--config`), and `scripts/test_check_code.py`
provides host-side unit tests for all scanner rules.

Default project rules live in:

```text
.codex/skills/code-check/config/default.json
```

## What to find

### 1. Kernel concurrency & locking (`src/`)

Find paths that can sleep while a non-sleepable lock is held or while local
interrupts are disabled, and configured lock-order inversions:

- `spin_lock_irqsave()` runs `cli` before acquiring the lock. If the holder
  then sleeps, it may never receive the wakeup IPI and may never resume.
- `memcpy_tofs`, `memcpy_fromfs`, `memcpy_fs`, `get_fs_long`, and `put_fs_long`
  touch user memory and can demand-page fault (`verify_area()` does not prevent
  later faults).
- `ring_buffer_read(..., 1)` and `ring_buffer_write(..., 1)` copy user memory.
  The `0` variants copy kernel memory.
- `sleep_on()` releases only the lock passed to it, not an outer spinlock.
- `tasklist_lock` and `mm_lock` are mutually non-nestable (`forbidden_pairs`);
  prefer atomic counters maintained under `mm_lock` when reading under
  `tasklist_lock`.

### 2. GUI / compositor / AF_UNIX client code (`app/`)

- **`gui_surface_upload.batch` requires `SO_SNDBUF` expansion**: AF_UNIX stream
  sockets default to a 4 KiB (`PAGE_SIZE`) ring buffer. Enabling 16 KiB/64 KiB
  `gui_surface_upload` batching without expanding `SO_SNDBUF` (e.g., to
  `GUI_SURFACE_UPLOAD_BATCH_BYTES`) fragments every chunk across ~4 `POLLOUT`
  rounds and causes interactive timeouts under load. Gate `.batch` on a
  successful `setsockopt(fd, SOL_SOCKET, SO_SNDBUF, ...)`.
- **Call `setsockopt(..., SO_SNDBUF, ...)` after `connect()` or `socketpair()`**:
  `sys_setsockopt` in `src/fs/socket.c` returns `-ENOTCONN` when `socket->pair`
  is still `NULL`.
- **Flush nonblocking client sockets before `poll()` wait**: After queueing
  handshake/control messages or window damage in `flush()`, drain immediately
  onto the nonblocking socket and re-check toolkit idle callbacks (`Fl::idle`)
  before sleeping until the next timer deadline.
- **Compositor child stdio & root-ramdisk protection**: Ordinary client spawn
  helpers (`wm_start_*`, excluding `wm_start_shell`) in `mario-wm` must call
  `wm_detach_client_stdio()`, and `terminal_drain_pty()` must never mirror raw
  PTY bytes to `STDOUT_FILENO`, or sustained output (e.g., `btop`) will fill
  `/tmp/mario-wm.log` on the 2.5 MiB root ramdisk (`ENOSPC`).

### 3. GUI, TTY, and shell test fixtures (`tests/`)

- **Wait for window/surface readiness before keyboard input**: After launching a
  GUI app asynchronously via desktop launcher (`GUI-CLICK` on a launcher asset)
  or background `SERIAL: ... &`, `pgrep` can succeed while the client is still
  loading fonts and `mario-desktop` still owns focus. Always wait for a visual
  window check (`GUI-CHECK-RECT: anywhere ...`, `GUI-CHECK-SCREEN-RECT: ...,anywhere`,
  taskbar active button) or explicit `window ready` log/file before sending
  `GUI-SEND-KEY`, `GUI-SEND-KEYS` (such as `meta_l,f` / `meta_l,w`), or
  `GUI-TYPE`. Note that checking menu dismissal alone (`GUI-CHECK-RECT` on the
  launcher popup column) does not prove the launched client created its window.
- **Isolate writable paths under `/tmp`**: Never mutate shared `/mnt` in a test
  unless that fixture explicitly mounts a test filesystem onto `/mnt`.
- **Position-independent assertions**: Follow `AGENTS.md` by using image-crop
  pointer actions (`GUI-CLICK`, `GUI-RIGHT-CLICK`, `GUI-POINT`) and `anywhere`
  rect checks rather than fixed coordinates or full-screen goldens unless window
  geometry, placement, occlusion, or cursor motion is itself under test.

## Workflow

1. Run the mechanical scanner across modified domains:

   ```sh
   python3 .codex/skills/code-check/scripts/check_code.py src app tests
   ```

2. For kernel changes under `src/`, also inspect every `spin_lock_irqsave`,
   `spin_lock`, or `ACQUIRE_LOCK` region for nested blocking calls (`sleep_on`,
   `schedule`) and stage user-memory copies in a kernel buffer outside the lock.

3. For GUI client or toolkit changes under `app/`, verify nonblocking socket
   writes, `SO_SNDBUF` ordering after `connect()`, and event-loop timeout
   handling when draw callbacks arm idle work.

4. Rebuild the matching bootable disk image (`make disk-image-vbe_1024x768`,
   `make disk-image-text`, or `make disk-image-gtk`) before running QEMU tests.

## Manual searches

```sh
rg -n "spin_lock_irqsave|spin_lock\(|ACQUIRE_LOCK" src
rg -n "memcpy_(tofs|fromfs|fs)|(get|put)_fs_long|verify_area" src
rg -n "\bsleep_on\b|\bschedule\b|wait_queue" src
rg -n "ring_buffer_(read|write)\([^;]*,\s*1\s*\)" src
rg -n "tasklist_lock|mm_lock\(|mm_unlock\(" src
rg -n "gui_surface_upload|SO_SNDBUF|wm_detach_client_stdio" app
rg -n "desktop-launcher|GUI-SEND-KEYS" tests/gui
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
occurs outside all spinlock regions. Do not flag unit-test fake upload sinks
under `app/test/` that exercise `gui_surface_upload` in memory without sockets.
Do not rewrite unrelated code unless the review or scanner identifies an actual
hazard.
