# Printk Ring Buffer and dmesg Plan

## Goal

Add a kernel ring buffer that captures every `printk()` message, and expose it
to userspace so the existing toybox `dmesg` command works through both:

- `/dev/kmsg` (toybox's default path),
- the `syslog`/`klogctl` syscall (toybox `-S` and clear/level options).

The console behavior of `printk()` remains unchanged by default: messages are
still written to the current text console and serial console while also being
stored in the ring.

## Current State

`printk()` is implemented in `src/kernel/printk.c`:

- `vsprintk()` formats into a private static `char buf[1024]`.
- `printk()` disables interrupts, formats the message, and immediately sends
  each character to `console_write_char()`.
- There is no retained log, no message priority, and no userspace interface.

The syscall table has `SYS_syslog` (103) wired to `sys_not_exist`:

```text
src/kernel/entry.S: .long sys_not_exist /* SYS_syslog */
```

Toybox `dmesg` source in `app/usr/toybox-0.8.9/toys/lsb/dmesg.c`:

- First tries `open("/dev/kmsg", O_RDONLY | O_NONBLOCK)`.
- Falls back to `klogctl()`/`syslog(2)` if `/dev/kmsg` is unavailable.
- Uses `klogctl(10)` to query size, `klogctl(3/4)` to read, `klogctl(5)` to
  clear, and `klogctl(8)` to set the console log level.

## Ring Buffer Requirements

The ring buffer must:

- Be allocated statically in the kernel image. `printk()` is called before the
  page allocator is ready, so it cannot use `page_alloc()` or `kmalloc()`.
- Preserve whole messages. Overwriting the oldest byte of a message must drop
  entire records, not leave a partial prefix.
- Be safe from interrupt context and from process context on the current
  uniprocessor i386 kernel. The existing `irq_save()`/`irq_restore()` approach
  is sufficient when combined with a simple spinlock.
- Record enough metadata for both `/dev/kmsg` and old `syslog(2)` formats:
  priority, sequence number, microsecond timestamp, flags, and text.

The existing `src/lib/ring_buffer.c` is not a good fit because it is
byte-oriented, dynamically allocates one page, and has no record boundaries or
metadata.

## New Ring Buffer Design

### Data Structure

Add `src/include/logbuf.h` and `src/kernel/logbuf.c`.

Use a static byte array with a record header:

```c
#define LOG_BUF_LEN    (16 * 1024)
#define LOG_LINE_MAX   1024

struct log_record {
    uint32_t len;      /* text length, excluding NUL */
    uint16_t prio;     /* LOG_* priority/facility, default LOG_INFO */
    uint16_t flags;
    uint32_t seq;
    uint64_t usec;     /* boot-time monotonic microseconds */
    char text[];
};
```

Static state:

```c
static char log_buf[LOG_BUF_LEN];
static unsigned long log_head;  /* next write offset */
static unsigned long log_tail;  /* oldest complete record */
static uint32_t log_next_seq;
static spinlock_t logbuf_lock;
static wait_queue_t kmsg_wait;
```

`logbuf_init()` is called from `printk_init()` after the console is set up.
The static zero initialization is also safe if an early `printk()` happens
before `logbuf_init()`.

### Storing a Message

API:

```c
void logbuf_store(int prio, const char *text, size_t len);
```

Algorithm:

1. Truncate `text` to `LOG_LINE_MAX`.
2. Compute the total record size including header and trailing NUL.
3. With interrupts disabled and `logbuf_lock` held:
   - While the free space is too small, advance `log_tail` over whole records.
   - If a single record is larger than the whole buffer, truncate it further
     and record a dropped-message marker.
   - Append the record at `log_head`, handling wrap-around.
   - Assign `seq = log_next_seq++`.
   - Set `usec = jiffies * (1000000 / HZ)`.
4. Release the lock and restore interrupts.
5. Call `wake_up_all(&kmsg_wait)` outside the lock.

The default priority is `LOG_INFO (6)`. Later, level-aware `printk` macros can
pass `LOG_ERR`, `LOG_WARNING`, etc.

### Reading Records

Expose helpers used by both `/dev/kmsg` and `sys_syslog`:

```c
int logbuf_read_seq(uint32_t seq, char *buf, size_t size,
                    uint32_t *next_seq);
uint32_t logbuf_oldest_seq(void);
uint32_t logbuf_next_seq(void);
size_t logbuf_formatted_syslog_len(void);
int logbuf_format_syslog(char *buf, size_t size, int clear);
void logbuf_clear(void);
int logbuf_console_loglevel(int set, int level);
```

`logbuf_read_seq()`:

- Finds the oldest record whose sequence is `seq`.
- Formats it in `/dev/kmsg` form:

  ```text
  <prio>,<seq>,<usec>,<flags>; <text>\n
  ```

- Writes at most `size` bytes.
- Returns the next sequence through `next_seq`.
- If `seq` is older than the oldest retained record, returns `-EPIPE`; toybox
  handles `EPIPE` by retrying.

`logbuf_format_syslog()` formats all retained records in the old syslog form:

```text
  <prio>[<seconds>.<microseconds>] <text>\n
```

If `clear` is true, it clears the ring after copying.

### Clearing

`logbuf_clear()` sets `log_head = log_tail = 0`. Sequence numbers may either
continue or reset; the first version can reset them and rely on open kmsg
cursors resetting to the new oldest sequence.

## printk Changes

Modify `src/kernel/printk.c`:

- Keep the existing console path.
- After `vsprintk()` produces the message, call
  `logbuf_store(LOG_INFO, buf, n)`.
- Keep the existing `irq_save()` window around formatting and console output.
- Store the message before console output so the ring is accurate even if a
  later console operation fails.

No change is needed to call sites. The default priority is `LOG_INFO`.

## `/dev/kmsg` Character Device

### Device Node

Use Linux's conventional major/minor:

```c
#define KMSG_MINOR 11
```

Add to `src/include/fs/chrdev.h` and `rd/device_list`:

```text
kmsg C 1 11
```

Register it through the existing memory character-device major in
`src/fs/char/mem.c`:

```c
case MEM_MINOR_KMSG:
    f->f_op = &kmsg_fops;
```

Add `src/fs/char/kmsg.c`.

### Open

`kmsg_open()` sets `file->f_pos` to `logbuf_oldest_seq()`. This treats
`f_pos` as the next sequence number rather than a byte offset.

No dynamic per-file allocation is required for the first version.

### Read

`kmsg_read()`:

1. Check pending signals and return `-ERESTARTSYS` if needed.
2. Lock the log buffer.
3. Try `logbuf_read_seq(file->f_pos, ...)`.
4. If no record is available:
   - Return `-EAGAIN` when `O_NONBLOCK` is set.
   - Otherwise sleep on `kmsg_wait` and retry.
5. On success, advance `file->f_pos` to the returned next sequence.
6. Return the number of bytes copied.

Reads do not remove records from the ring. Ring overwrite may make an open
cursor stale; that case returns `-EPIPE`, and the next read restarts at the
oldest record.

### Write

Optional but useful:

- Copy the user buffer into a temporary kernel buffer.
- If it starts with `<n>`, parse `n` as the priority.
- Call `logbuf_store()`.

This allows:

```text
echo hello > /dev/kmsg
dmesg | tail -1
```

The first version may implement write as a stretch goal.

### Lseek

`kmsg_lseek()` can support:

- `SEEK_SET 0`: set cursor to the oldest record.
- `SEEK_END 0`: set cursor to `logbuf_next_seq()`.

`SEEK_DATA`/`SEEK_HOLE` support is not required for toybox `dmesg` because it
calls `lseek(fd, 0, SEEK_DATA)` and ignores the result. It can be deferred.

## `sys_syslog` / `klogctl`

Add `src/kernel/syslog.c` with:

```c
int sys_syslog(int type, char *buf, int len);
```

Update `src/kernel/entry.S`:

```text
.long sys_syslog /* SYS_syslog */
```

Supported actions:

| Type | Action | Implementation |
|---|---|---|
| 3 | `SYSLOG_ACTION_READ_ALL` | Copy all formatted records without clearing. |
| 4 | `SYSLOG_ACTION_READ_CLEAR` | Copy all records, then clear the ring. |
| 5 | `SYSLOG_ACTION_CLEAR` | Clear the ring. |
| 8 | `SYSLOG_ACTION_CONSOLE_LEVEL` | Get/set console log level. |
| 9 | `SYSLOG_ACTION_SIZE_UNREAD` | Return formatted unread size. |
| 10 | `SYSLOG_ACTION_SIZE_BUFFER` | Return formatted buffer size. |

For actions 3/4/9/10, validate the user buffer with `verify_area()` and copy
through `memcpy_tofs()`.

Action 8 behavior:

- If `len` is in the valid range 1..8, set the console log level and return 0.
- Otherwise return the current console log level.

Add a kernel `console_loglevel` variable, defaulting to `LOG_DEBUG (7)`. The
existing `printk` path can compare the default message priority against this
level before writing to the console, while always storing the message in the
ring.

Actions 6/7 (`CONSOLE_OFF`/`CONSOLE_ON`) and action 2 (`READ`) can be added
later if `klogd` support is needed.

## Files to Change

### New

- `src/include/logbuf.h`
- `src/kernel/logbuf.c`
- `src/fs/char/kmsg.c`
- `src/kernel/syslog.c`

### Modified

- `src/kernel/printk.c`
- `src/kernel/entry.S`
- `src/fs/char/mem.c`
- `src/include/fs/chrdev.h`
- `rd/device_list`
- `doc/devices.txt`

No build-list change is required for the new C files.

## Milestones

### Milestone 1: Ring buffer only

- Add static record-oriented ring buffer.
- Integrate `logbuf_store()` into `printk()`.
- Keep console output unchanged.
- Add debug hooks or a temporary `/sys/kernel/debug/log` file to verify that
  messages are retained.

### Milestone 2: `/dev/kmsg` read support

- Add `kmsg` device node.
- Implement `kmsg_open`/`kmsg_read`.
- Support `O_NONBLOCK` and blocking reads.
- Verify:

  ```text
  dmesg
  dmesg | tail
  ```

### Milestone 3: syslog/klogctl support

- Implement `sys_syslog` actions 3, 4, 5, 8, 9, and 10.
- Wire `SYS_syslog` in the syscall table.
- Verify:

  ```text
  dmesg -S
  dmesg -c
  dmesg -C
  dmesg -n 4
  ```

### Milestone 4: kmsg write, lseek, and hardening

- Add `/dev/kmsg` write support.
- Add `kmsg_lseek()` for `SEEK_SET`/`SEEK_END`.
- Add dropped-message accounting and overflow checks.
- Add a small test program or shell test for wrap-around behavior.

## Validation

After boot:

```text
dmesg | head
dmesg | grep -i mario
dmesg -S | head
```

Expected:

- Boot-time `printk` messages such as `Ramdisk(s):` and `Serial device(s):`
  appear.
- Repeated `dmesg` output is stable until `-c` or `-C` clears the buffer.
- `dmesg -c` prints and clears the buffer.
- `dmesg -n 4` changes the kernel console log level without crashing.

Shell test candidate:

- `tests/shell/dmesg.test`
  - Run `dmesg`, check for a known boot string.
  - Run `dmesg -S`, check the same string.
  - Run `dmesg -C`, then verify `dmesg` has little or no output.
  - Write a message to `/dev/kmsg`, then verify it appears in `dmesg`.

## Deferred

- Full Linux `/dev/kmsg` seek/sequence semantics with `SEEK_DATA` and clear
  points.
- `/proc/kmsg` and `klogd` streaming.
- Level-aware `printk(KERN_ERR ...)` macros and call-site priority migration.
- Record timestamps using a high-resolution monotonic clock instead of PIT
  jiffies.
- Ring buffer resizing or persistent logging.
- Structured `dev_*` printk records.

## Risks

- `printk()` runs before the page allocator, so the ring must be static and
  must not call allocation or sleeping functions.
- Appending from interrupt context and reading from process context must use
  the same locking protocol; otherwise records can be torn or indices corrupt.
- Partial overwrite must be prevented by dropping whole records.
- Toybox `dmesg` expects specific `/dev/kmsg` and `syslog(2)` text formats;
  small formatting differences can make messages appear empty or unparsed.
- Blocking `/dev/kmsg` reads must wake when `printk` appends, including
  appends from interrupt context.
