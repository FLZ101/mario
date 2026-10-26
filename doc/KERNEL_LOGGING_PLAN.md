# Kernel Log Level API Plan

## Goal

Add level-aware kernel logging to Mario so call sites can explicitly mark a
message as an error, warning, notice, info, or debug message. The existing
ring buffer and userspace interfaces already understand priorities; this plan
only makes the kernel-side call sites and console filtering use them.

The old `doc/PRINTK_RING_BUFFER_DMESG_PLAN.md` listed this work as out of
scope. This plan supersedes that item.

## Current State

The ring buffer side is already implemented:

- `src/include/logbuf.h` defines `LOG_EMERG` through `LOG_DEBUG`.
- `struct log_record` already stores `uint16_t prio`.
- `logbuf_store()` in `src/kernel/logbuf.c` records that priority.
- `/dev/kmsg` and `sys_syslog()` already format the stored priority.
- `logbuf_console_loglevel()` supports getting and setting the console log
  level through `syslog(2)` action 8.

The level-aware API itself is now present:

- `src/include/printk.h` defines `KERN_*`, `pr_*`, and `pr_task_*` macros.
- `src/include/misc.h` includes `printk.h`, and `hang()` uses `KERN_CRIT`.
- `src/kernel/printk.c` parses a leading `<N>` prefix and uses that priority
  for both ring-buffer storage and console filtering.
- `sprintkn()` and `vsprintkn()` provide bounded formatting; `printk()` uses
  `vsprintkn(buf, sizeof(buf), ...)` instead of the unbounded formatter.

The remaining work is to refine existing usages:

- Existing `printk()` call sites are still bare `printk()` calls, so they are
  all stored and displayed as `LOG_INFO` even though many are errors, warnings,
  or debug diagnostics.
- Existing `sprintk()` call sites are unbounded. Several write repeatedly into
  fixed-size stack buffers with `sprintk(buf + n, ...)` and should migrate to
  `sprintkn()`/`vsprintkn()` once the buffer size is threaded through.
- `dmesg -n LEVEL` now works for new `pr_*` messages, but has no visible
  effect on unclassified legacy call sites because they still default to
  `LOG_INFO`.

The current console filtering rule is:

```c
if (prio < logbuf_console_loglevel(0, 0))
    /* write the formatted text to the console */
```

Messages are always stored in the ring buffer. The comparison only controls
whether they are also written to the current console.

## Target Semantics

Provide the standard Linux-style kernel logging macros:

| Macro | Priority | Meaning |
|---|---:|---|
| `pr_emerg` | `LOG_EMERG` | System is unusable |
| `pr_alert` | `LOG_ALERT` | Action must be taken immediately |
| `pr_crit` | `LOG_CRIT` | Critical conditions |
| `pr_err` | `LOG_ERR` | Error conditions |
| `pr_warn` / `pr_warning` | `LOG_WARNING` | Warnings |
| `pr_notice` | `LOG_NOTICE` | Normal but significant conditions |
| `pr_info` | `LOG_INFO` | Informational messages |
| `pr_debug` | `LOG_DEBUG` | Debugging messages |

Requirements:

- Bare `printk()` keeps its current behavior and defaults to `LOG_INFO`.
- `printk(KERN_ERR "...")` and `pr_err("...")` must store and optionally
  print with priority `LOG_ERR`, without leaking the `<3>` prefix into the
  visible text.
- Messages are always stored in the ring buffer, subject to the existing
  truncation and overflow policy.
- Console output continues to use `prio < console_loglevel`.
- The default console log level remains `LOG_DEBUG (7)`. With the current
  strict `<` comparison this means debug messages are stored but not printed
  to the console by default; `dmesg -n 8` can enable them.
- `dmesg -r` and `dmesg -S -r` must show the assigned priority, not always 6.

## Design

### 1. Logging macros

The header `src/include/printk.h` is already added:

```c
#define KERN_EMERG   "<0>"
#define KERN_ALERT   "<1>"
#define KERN_CRIT    "<2>"
#define KERN_ERR     "<3>"
#define KERN_WARNING "<4>"
#define KERN_NOTICE  "<5>"
#define KERN_INFO    "<6>"
#define KERN_DEBUG   "<7>"

#define pr_emerg(fmt, ...)  printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)  printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)   printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)    printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)   printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) pr_warn(fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...) printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)   printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)  printk(KERN_DEBUG fmt, ##__VA_ARGS__)
```

Include `src/include/printk.h` from `src/include/misc.h` so every existing
`printk()` caller can use the macros without adding include lines. Keeping the
macros in a separate header makes the logging API easier to find and keeps
`misc.h` focused on low-level utility macros.

### 2. Prefix parsing in `printk()`

`src/kernel/printk.c` now parses an optional `<N>` prefix before formatting:

```c
int printk(const char *fmt, ...)
{
    int prio = LOG_INFO;

    if (fmt[0] == '<') {
        const char *p = fmt + 1;
        int value = 0;

        while ('0' <= *p && *p <= '9') {
            value = value * 10 + (*p - '0');
            p++;
        }
        if (p != fmt + 1 && *p == '>') {
            prio = value & 7;  /* keep the priority, ignore facility bits */
            fmt = p + 1;
        }
    }

    irq_save();
    va_start(ap, fmt);
    n = vsprintkn(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logbuf_store(prio, buf, n);
    ...
}
```

Only the leading `<N>` marker is stripped. The parser must not scan later text
for a priority, and it must fall back to `LOG_INFO` for malformed input such
as `<` or `<abc>`.

The parser is deliberately independent of the formatting core. No
format-specifier changes are required.

### 3. Bounded formatting

`src/kernel/printk.c` already provides safe string-formatting entry points:

```c
int sprintkn(char *buf, size_t size, const char *fmt, ...);
int vsprintkn(char *buf, size_t size, const char *fmt, va_list ap);
```

Semantics:

- At most `size - 1` formatted characters are written, followed by a NUL.
- A zero buffer size writes nothing and returns 0.
- The return value is the number of formatted characters written, not the
  would-have-been length used by standard `snprintf()`.
- `printk()` uses `vsprintkn(buf, sizeof(buf), ...)` so its static buffer
  cannot overflow even for a long or malformed format string.

Legacy `sprintk()` remains for compatibility, but new or touched call sites
must use `sprintkn()` when the destination capacity is known.

### 4. `hang()` and `assert()`

`hang()` in `src/include/misc.h` already uses `KERN_CRIT`:

```c
#define hang(...) do { \
    printk(KERN_CRIT __VA_ARGS__); \
    __asm__ __volatile__ ("cli; 1:hlt; jmp 1b"); \
} while (0)
```

`assert()` already delegates to `hang()`, so assertion failures automatically
become critical messages.

### 5. Call-site classification

The current `printk()` call sites should be migrated to the smallest
appropriate macro. The initial mapping is:

| File | Target macro |
|---|---|
| `src/include/misc.h` `hang()` | `KERN_CRIT` via `printk()` |
| `src/mm/fault.c` OOM path | `pr_warn` |
| `src/mm/fault.c` page-fault diagnostic | `pr_debug` |
| `src/mm/page_alloc.c` free-area dump | `pr_debug` |
| `src/mm/e820.c` memory map dump | `pr_info` |
| `src/mm/e820.c` "Too many entries" | `pr_err` |
| `src/mm/mm.c` `print_mmap()` | `pr_debug` |
| `src/fs/exec.c` loader failures | `pr_err` |
| `src/fs/char/serial.c` serial list | `pr_info` |
| `src/fs/block/rd.c` ramdisk list | `pr_info` |
| `src/fs/block/ahci.c` disk list | `pr_info` |
| `src/kernel/sys.c` "Not implemented" | `pr_warn` |
| `src/kernel/trap.c` `print_tr()` | `pr_debug` |
| `src/kernel/trap.c` NMI warning | `pr_warn` |
| `src/kernel/sched.c` runqueue dump | `pr_debug` |
| `src/fs/mariofs/file.c` corrupt filesystem | `pr_err` |
| `src/fs/mariofs/super_block.c` corrupt filesystem | `pr_err` |
| `src/fs/char/console.c` `DEBUG_PRINTK()` | `pr_debug` |

Keep a plain `printk()` only where the caller intentionally wants the default
priority or where the message is emitted before the level-aware macros matter.
After migration there should be no remaining bare `printk()` that clearly
represents an error, warning, or debug diagnostic.

### Safe-buffer migration for `sprintk()`

In the same refinement pass, convert fixed-buffer users of `sprintk()` to
`sprintkn()` or `vsprintkn()`. Priority targets are:

| File | Buffer work |
|---|---|
| `src/fs/proc.c` | Many `sprintk(buf + n, ...)` chains write into `content[512]`, `line[PATH_MAX + 128]`, `source[64]`, and `name[16]`. Thread the known size through the helper functions and use `sprintkn(buf + n, size - n, ...)`. |
| `src/fs/debugfs.c` | `line[192]` and several small stack buffers are filled with repeated `sprintk(buf + n, ...)` calls. Replace with size-aware `sprintkn()` calls; keep the existing explicit `size` checks for whole-file truncation. |
| `src/fs/sysfs.c` | `name[32]` is written directly by `sprintk()`. Use `sprintkn(name, sizeof(name), ...)` and check for truncation where names must fit the directory entry. |
| `src/fs/char/console.c` | The cursor-position reply uses `resp[32]`. Replace with `sprintkn(resp, sizeof(resp), ...)`. |

Rule for migration:

```c
n = sprintkn(buf, size, "prefix %d\n", value);
n += sprintkn(buf + n, size - n, "suffix %s\n", name);
```

The size must always be the remaining capacity of the same buffer. Functions
that already take `size` but ignore it, such as `proc_blkdev_name()` and
`proc_file_content()`, must start honoring it instead of using unbounded
`sprintk()`.

### 6. Console filtering behavior

No changes are required in `logbuf.c` for basic support. The existing
`prio < logbuf_console_loglevel(0, 0)` check in `printk()` automatically
honors the parsed priority.

Consider documenting the semantics in `logbuf.h`:

```c
/* console_loglevel is a threshold: messages with prio < level are printed */
```

The default remains 7. Error/warning/info/notice messages are visible on the
console, while debug messages are retained in the ring and can be enabled with
`dmesg -n 8`.

### 7. Automatic essential logs

Besides migrating existing call sites, add a small set of always-available
lifecycle logs at kernel boundaries that are commonly needed for debugging.
These are not per-read/per-write or per-page operations; they should only
fire on discrete, high-value events such as boot, process creation, process
exit, fatal traps, filesystem mount, and allocation failure.

Add task-context convenience macros in `src/include/printk.h`:

```c
#define pr_task_err(fmt, ...) \
    pr_err("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_warn(fmt, ...) \
    pr_warn("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_info(fmt, ...) \
    pr_info("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
#define pr_task_debug(fmt, ...) \
    pr_debug("pid %d %s: " fmt, current->pid, current->comm, ##__VA_ARGS__)
```

These macros assume the call site is in task context. Do not use them from
interrupt handlers or before `current` is meaningful.

Initial automatic hook points:

| Hook | Location | Level |
|---|---|---|
| Init executable cannot start | `src/kernel/mario.c` `init()` | existing `hang()` becomes `pr_crit` |
| Fork resource allocation fails | `src/kernel/fork.c` failure paths | `pr_task_err("fork failed: no memory\n")` |
| Kernel-mode fatal trap | `src/kernel/trap.c` `die_if_kernel()` | `pr_crit("Oops: %s err=%d pid=%d\n", str, err, current->pid)` |
| Mount succeeds | `src/fs/super_block.c` `do_mount()` | `pr_info("mount dev=%x dir=%s type=%s\n", dev, dir, type)` |
| Mount fails | `src/fs/super_block.c` `do_mount()` error path | `pr_err("mount failed dev=%x dir=%s err=%d\n", ...)` |
| Unmount succeeds | `src/fs/super_block.c` `sys_umount()` | `pr_info("umount %s\n", dir)` |
| Page allocation fails | `src/mm/page_alloc.c` `alloc_pages()` | `pr_err("alloc_pages order=%u failed\n", order)` |
| `kmalloc()` fails | `src/mm/kmalloc.c` | `pr_err("kmalloc size=%u failed\n", size)` |

Use `pr_task_*` for process-lifecycle events and plain `pr_*` for global
events such as mounts and allocator failures. Keep each message to one line so
the existing `LOG_LINE_MAX` limit is not hit and toybox `dmesg` remains easy
to read.

The existing device-discovery messages for serial, ramdisk, AHCI, and memory
layout remain informational logs and do not need duplicate automatic hooks.

## Files to Change

New files:

- `src/include/printk.h` (already added)

Modified files:

- `src/include/misc.h`: include `printk.h`; update `hang()` (already done).
- `src/kernel/printk.c`: parse `<N>`, use the parsed priority, and add
  `sprintkn()`/`vsprintkn()` (already done).
- Call sites listed in the classification table above.
- Safe-buffer migration in `src/fs/proc.c`, `src/fs/debugfs.c`,
  `src/fs/sysfs.c`, and `src/fs/char/console.c`.
- Automatic hooks in `src/kernel/mario.c`, `src/kernel/fork.c`,
  `src/kernel/exit.c`, `src/kernel/trap.c`, `src/fs/exec.c`,
  `src/fs/super_block.c`, `src/mm/page_alloc.c`, and `src/mm/kmalloc.c`.
- `tests/shell/dmesg.test`: add priority checks where useful.

No syscall-table, device-node, or build-list changes are needed because this
builds on the existing `/dev/kmsg`, `syslog`, and ring-buffer implementation.

## Milestones

### Milestone 1: API and parser

Status: implemented.

- `src/include/printk.h` is added and included from `src/include/misc.h`.
- `src/kernel/printk.c` parses `<N>` and uses the parsed priority.
- `hang()` uses `KERN_CRIT`.
- `sprintkn()`/`vsprintkn()` are available and `printk()` is bounded.

Validation:

- Build succeeds.
- Existing `dmesg` test still passes.
- Boot logs such as `Ramdisk(s):` still appear in `dmesg`.

### Milestone 2: Migrate call sites

- Convert the call sites listed above to `pr_err`, `pr_warn`, `pr_info`, or
  `pr_debug`.
- Convert fixed-buffer `sprintk()` users to `sprintkn()`/`vsprintkn()`.
- Keep startup tables informational and noisy diagnostics debug-only.

Validation:

- `dmesg -r` shows priorities other than 6 for classified messages.
- Console remains usable; page-fault and VMA dumps do not spam the console by
  default because they are debug-level.
- A corrupted MarioFS error still appears on the console as an error.
- Proc and debugfs outputs remain within their stack buffers and still pass
  their existing shell checks.

### Milestone 3: Automatic lifecycle logs

If keeping the patch reviewable, land the automatic hooks separately:

- Add `pr_task_err`/`pr_task_warn`/`pr_task_info`/`pr_task_debug` in
  `src/include/printk.h` (already added).
- Add boot, fork, exit, exec, trap, mount/unmount, and allocation-failure
  logs.
- Verify each message is one line and does not fire on hot paths such as
  normal page faults or regular file I/O.

Validation:

- Boot `mario`, then run `dmesg -r` and confirm both priority values and
  lifecycle messages appear.
- Run `sh`, a few fork/exec commands, and a mount command; confirm the logs
  are useful but do not flood the ring with repeated per-operation lines.

### Milestone 4: Tests and docs

- Add or extend a shell test that:
  - writes a priority-prefixed message to `/dev/kmsg`,
  - verifies `dmesg -r` preserves the priority,
  - runs `dmesg -n 4` and confirms the level is accepted.
- Add a short README section in `doc/SESSION_NOTES.md` or the new plan that
  documents which macro to use for which message class.

Validation:

- `make test` or the targeted `tests/shell/dmesg.test` passes.
- A test boot with `dmesg -n 8` does not crash.

## Test Plan

Shell test `tests/shell/dmesg.test` should be extended with:

```text
RUN: echo '<3>mario-kmsg-err' > /dev/kmsg
RUN: dmesg -r > /root/dmesg.prio.out; if grep -q '<3>mario-kmsg-err' /root/dmesg.prio.out; then echo prio-ok; else echo prio-missing; fi
CHECK: prio-ok
```

The `/dev/kmsg` write path already parses `<N>`, so this directly tests that
priority survives through the ring buffer and raw dmesg formatting.

For kernel-side macros, a useful follow-up test is a debugfs read-only file
that emits one `pr_err`, one `pr_warn`, one `pr_info`, and one `pr_debug`
message on demand. Reading the file and then checking `dmesg -r` for those
priorities would exercise the `printk()` parser end to end without adding a
dedicated syscall. This can be added in Milestone 4.

## Out of Scope

- Dynamic debug, per-subsystem debug flags, or `pr_devel` compile-time
  stripping.
- `printk_once`, `printk_ratelimited`, and rate limiting.
- Structured records, dev_printk, or dictionary data.
- `panic()`/`oops` redesign beyond using the new `pr_crit`/`pr_emerg` macros.
- Changing the ring buffer size or record format.

## Risks

- **Boot-time output changes.** Early boot messages are still formatted
  through `printk()`. The `<N>` parser must work before `logbuf_init()` and
  before the console log level is explicitly set.
- **Too-aggressive debug migration.** Page-fault diagnostics are useful during
  bring-up but noisy on the console. They should be `pr_debug`, not `pr_info`,
  while still being retained in the ring for later inspection.
- **Console threshold convention.** Current code prints when
  `prio < console_loglevel`, so `LOG_DEBUG` messages are suppressed at the
  default level 7 and require level 8. Keep that behavior to avoid changing
  existing console output unexpectedly.
- **Bare `printk()` remains LOG_INFO.** Call-site migration must be explicit;
  simply adding the macros does not change existing unclassified messages.
