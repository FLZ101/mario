# Debugging the intermittent `tests/shell/sbase.mkfifo.test` hang

This document records how the FIFO shell test was reproduced, narrowed
down, and fixed. The final fix has two parts:

1. A lost-wake-up bug in `src/fs/pipe.c` made blocking FIFO opens sleep
   again after their partner had already appeared and disappeared.
2. An exec/TLS stale-`%gs` bug in `src/fs/exec.c` could cause a
   general-protection fault in `_switch_to()` when a task was scheduled
   before its syscall return restored `%gs` from the trap frame.

## 1. Symptom

Run the single test:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell --test sbase.mkfifo.test
```

The failure is intermittent. Before the fix, the run usually stopped here:

```text
RUN: printf 'recreated fifo\n' > fifo & cat fifo
CHECK: recreated fifo
```

and `pexpect` reported a timeout. The shell test output often ended around:

```text
printf 'recreated fifo\\n' > fifo & cat fifo

[1] 25
```

Sometimes the failure was not a silent hang but a kernel general-protection
fault:

```text
[48] general protection
...
eip=0x8010b068,  cs=0x00000010
...
gs=0x00000033
```

The same test would pass on some runs, which pointed to a scheduling race
rather than a deterministic FIFO command failure.

## 2. Reproduction checklist

Build the kernel and ramdisk:

```sh
make -j$(nproc)
```

Run the single failing test once:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell --test sbase.mkfifo.test
```

Run it repeatedly to expose the intermittent failure:

```sh
for i in $(seq 1 50); do
  timeout 20 .venv/bin/python tests/lit_shell_test.py tests/shell \
    --test sbase.mkfifo.test || {
      echo "failed at iteration $i"
      exit 1
    }
done
```

The test that matters is the remove/recreate sequence in
`tests/shell/sbase.mkfifo.test`:

```text
RUN: rm fifo; mkfifo fifo; echo recreate-status:$?
RUN: test -p fifo; echo recreate-mode:$?
CHECK: recreate-status:0
CHECK: recreate-mode:0

RUN: printf 'recreated fifo\n' > fifo & cat fifo
CHECK: recreated fifo
```

The background writer opens `fifo` first, the foreground reader opens it
second, and then the writer writes and closes. If the reader is not
scheduled again until after the writer has already closed, the old open
loop could put the reader back to sleep forever.

## 3. Relevant kernel code

### 3.1 FIFO open waiting

The old blocking open logic in `src/fs/pipe.c` looked like this:

```c
if (f->f_mode & 1) {
    info->n_reader++;
    f->f_op = &read_pipe_fops;
    wake_up_interruptible(&info->wait_write);
    while (!info->n_writer && !(f->f_flags & O_NONBLOCK)) {
        sleep_on(&info->wait_read, TASK_INTERRUPTIBLE, &info->lock);
        ACQUIRE_LOCK(&info->lock);
        if (current->signal & ~current->blocked) {
            info->n_reader--;
            wake_up_interruptible(&info->wait_write);
            error = -ERESTARTSYS;
            break;
        }
    }
    RELEASE_LOCK(&info->lock);
    return error;
}
```

The writer side had the symmetric problem:

```c
info->n_writer++;
f->f_op = &write_pipe_fops;
wake_up_interruptible(&info->wait_read);
while (!info->n_reader && !(f->f_flags & O_NONBLOCK)) {
    sleep_on(&info->wait_write, TASK_INTERRUPTIBLE, &info->lock);
    ACQUIRE_LOCK(&info->lock);
    if (current->signal & ~current->blocked) {
        info->n_writer--;
        wake_up_interruptible(&info->wait_read);
        error = -ERESTARTSYS;
        break;
    }
}
if (!info->n_reader && !error)
    error = -ENXIO;
if (error == -ENXIO)
    info->n_writer--;
```

The wait loops are re-checking `n_reader`/`n_writer` after `sleep_on()`
returns. That check is the bug.

### 3.2 Sleep and wake-up semantics

`src/kernel/sched.c` implements the wait queue primitives:

- `sleep_on()` puts `current` on a wait queue, sets its state, releases
  the caller's spinlock, and calls `schedule()`.
- `wake_up()` marks matching waiters as `TASK_RUNNING` and puts them on the
  runqueue, but it does **not** immediately run them.

Because this is a single-CPU scheduler, a woken task can stay on the
runqueue while the waker continues running, writes to the FIFO, and closes
it.

### 3.3 Exec TLS cleanup and context switching

`flush_old_exec()` in `src/fs/exec.c` clears TLS on exec:

```c
// Clear TLS !!!
irq_save();
tr->gs = 0;
current->thread.gs = 0;
struct user_desc *ud = current->thread.user_descs;
for (int i = 0; i < NR_USER_DESC; ++i) {
    ud[i] = (struct user_desc){0};
}
irq_restore();
```

Before the fix, it did **not** load `%gs` with zero immediately. The actual
`%gs` register was still the old user TLS selector until the syscall return
path restored it from `tr->gs`.

`ret_common` in `src/kernel/entry.S` may call `schedule()` before
`RESTORE_ALL`. If that happens, `_switch_to()` in `src/kernel/sched.c`
saves the live `%gs` back into `current->thread.gs`, overwriting the zero
that `flush_old_exec()` just stored:

```c
save_segment(gs, prev->gs);
load_segment(gs, next->gs);
```

The TLS GDT entries for that task were already cleared. Later, switching
back to that task reloads the stale selector `0x33` while its GDT TLS
descriptor is zeroed, which causes a general-protection fault.

## 4. Step 1: fix the FIFO lost wake-up

Change both blocking open paths in `src/fs/pipe.c` to sleep **exactly
once**. A wake-up means the partner existed at some point, even if it has
already closed by the time this task runs again.

The reader branch becomes:

```c
if (f->f_mode & 1) {
    info->n_reader++;
    f->f_op = &read_pipe_fops;
    /* Let any writer waiting for a reader continue. */
    wake_up_interruptible(&info->wait_write);
    if (!info->n_writer && !(f->f_flags & O_NONBLOCK)) {
        /*
         * Sleep exactly once.  The wake-up means a writer existed,
         * even if it already closed before this reader runs again.
         * Rechecking n_writer here would turn that valid rendezvous
         * into a lost wake-up and deadlock the open.
         */
        sleep_on(&info->wait_read, TASK_INTERRUPTIBLE, &info->lock);
        ACQUIRE_LOCK(&info->lock);
        if (current->signal & ~current->blocked) {
            info->n_reader--;
            wake_up_interruptible(&info->wait_write);
            error = -ERESTARTSYS;
        }
    }
    RELEASE_LOCK(&info->lock);
    return error;
}
```

The writer branch becomes:

```c
info->n_writer++;
f->f_op = &write_pipe_fops;
/* Let any reader waiting for a writer continue. */
wake_up_interruptible(&info->wait_read);
if (!info->n_reader) {
    if (f->f_flags & O_NONBLOCK) {
        info->n_writer--;
        error = -ENXIO;
    } else {
        /*
         * As with read opens, a single wake-up is enough to complete
         * a blocking write open: the partner may have opened and then
         * closed before this task was scheduled again.
         */
        sleep_on(&info->wait_write, TASK_INTERRUPTIBLE, &info->lock);
        ACQUIRE_LOCK(&info->lock);
        if (current->signal & ~current->blocked) {
            info->n_writer--;
            wake_up_interruptible(&info->wait_read);
            error = -ERESTARTSYS;
        }
    }
}
RELEASE_LOCK(&info->lock);
return error;
```

Nonblocking write open still returns `-ENXIO` when there is no reader.

## 5. Step 2: reproduce and inspect the general-protection fault

After applying only the FIFO open fix, the original hang may disappear but
the test can still fail with the kernel GP fault shown above.

Map the faulting EIP to the kernel source:

```sh
nm -n src/kernel/kernel.exe | grep -E 'switch_to|schedule'
objdump -d src/kernel/kernel.exe | grep -C5 8010b068
```

The instruction at `0x8010b068` is in `_switch_to()`:

```asm
mov    0x10(%eax),%gs
```

It is reloading the next task's saved `%gs` selector.

## 6. Step 3: fix exec TLS `%gs` synchronization

In `src/fs/exec.c`, make the live `%gs` register consistent with the just
cleared TLS state before interrupts are restored:

```c
// Clear TLS !!!
irq_save();
tr->gs = 0;
current->thread.gs = 0;
struct user_desc *ud = current->thread.user_descs;
for (int i = 0; i < NR_USER_DESC; ++i) {
    ud[i] = (struct user_desc){0};
}
/*
 * Keep the live %gs register in sync with thread.gs.  If this task is
 * scheduled before the syscall return restores %gs from the trap frame,
 * _switch_to() would otherwise save the old TLS selector while the GDT
 * TLS entries have already been cleared, and reloading that selector
 * later faults with a general-protection exception.
 */
unsigned short zero = 0;
load_segment(gs, zero);
irq_restore();
```

This ensures any context switch before `RESTORE_ALL` saves `%gs = 0`, not
the stale user TLS selector.

## 7. Optional GDB instrumentation

These breakpoints are useful for repeating the diagnosis manually.

### Terminal A: QEMU with GDB stub

Use the same QEMU arguments as the test harness:

```sh
qemu-system-i386 \
  -s -S \
  -m 256M \
  -kernel src/kernel/kernel.exe \
  -initrd rd/rdx \
  -display none \
  -serial stdio \
  -no-reboot \
  -no-shutdown
```

### Terminal B: GDB

```sh
gdb -q src/kernel/kernel.exe
```

```gdb
target remote :1234
set pagination off
continue
```

### FIFO open tracing

```gdb
break src/fs/pipe.c:219
commands
silent
printf "FIFO_OPEN pid=%d mode=%x flags=%x readers=%u writers=%u\n", \
  current->pid, f->f_mode, f->f_flags, info->n_reader, info->n_writer
continue
end
```

Run the recreated-fifo command in QEMU:

```sh
printf 'recreated fifo\n' > fifo & cat fifo
```

In the bad case, the reader logs a second `FIFO_OPEN` or never returns from
the open path while `n_writer` has already gone back to zero.

### Context-switch / TLS tracing

```gdb
break _switch_to
commands
silent
printf "SWITCH prev=%d gs=%#x next=%d gs=%#x\n", \
  p->pid, p->thread.gs, n->pid, n->thread.gs
continue
end
```

If `next->thread.gs` is `0x33` while `next->thread.user_descs[0]` is zero,
the exec/TLS race is present.

## 8. Remove any temporary tracing

If a temporary serial-only `printk` was added while debugging, revert it:

```sh
git checkout -- src/kernel/printk.c
```

Then inspect the final patch:

```sh
git diff -- src/fs/pipe.c src/fs/exec.c src/kernel/printk.c
git diff --check
```

## 9. Build and verify

Build the kernel and ramdisk:

```sh
make -j$(nproc)
```

Run the originally failing test 50 times:

```sh
for i in $(seq 1 50); do
  timeout 20 .venv/bin/python tests/lit_shell_test.py tests/shell \
    --test sbase.mkfifo.test || {
      echo "failed at iteration $i"
      exit 1
    }
done
```

Expected result: all 50 runs pass.

Run the complete shell suite:

```sh
.venv/bin/python tests/lit_shell_test.py tests/shell
```

Expected result: every shell test passes, including:

```text
PASS: tests/shell/sbase.mkfifo.test
PASS: tests/shell/oksh-0.test
PASS: tests/shell/python-0.test
...
```

## 10. Why the final fix is minimal

The FIFO change only removes the erroneous re-check after a wake-up. It
does not change endpoint counting, ring-buffer behavior, or pipe read/write
data waiting.

The exec change only synchronizes the live `%gs` register with the TLS
state that was just cleared. It does not change how user programs install
TLS later through `set_thread_area()`.
