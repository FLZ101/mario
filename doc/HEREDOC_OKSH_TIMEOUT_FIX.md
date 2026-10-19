# Fix for Intermittent `heredoc.test` / `oksh-0.test` Timeouts

## Problem

`tests/shell/heredoc.test` and `tests/shell/oksh-0.test` usually finished in
about 1-5 seconds, but intermittently took more than 30 seconds and timed
out. Repeating either test showed the failure was timing-dependent rather
than caused by the shell command itself.

## Symptom

Run one of the affected tests repeatedly:

```sh
make CC=$PWD/util/mario-cc LD=$PWD/util/mario-ld -C src -j2
for i in $(seq 1 30); do
  timeout 40 .venv/bin/python tests/lit_shell_test.py \
    --test heredoc.test tests/shell || echo "failed at $i"
done
```

Before the fix, some runs passed quickly and others hung until the lit
runner timeout.

On a hung run, QEMU was still running and the CPU was stuck in
`get_buffer()`:

```text
EIP=0x80112771
```

The surrounding assembly is the `buffer_lock` spin loop:

```asm
80112771: xchg %eax,0x80182cb0   ; buffer_lock
80112777: and  %eax,%eax
80112779: jne  80112771
```

So the kernel was not making progress; it was spinning forever waiting for
the buffer lock.

## Root Cause

The kernel spinlock implementation in `src/include/lib/spinlock.h` only
performs an atomic exchange:

```c
#define ACQUIRE_LOCK(lock) \
do { \
	__asm__ __volatile__( \
		"1:\n" \
		"xchgl %0, %1\n\t" \
		"andl %1, %1\n\t" \
		"jnz 1b" \
		:"+m"((lock)->locked) \
		:"a"(1)); \
} while (0)
```

It does **not** disable interrupts. A task can therefore hold a spinlock
while a hardware interrupt is handled on the same CPU.

`get_buffer()` in `src/fs/block/buffer.c` acquires `buffer_lock`, then
releases it only after finishing the buffer lookup:

```c
try:
	ACQUIRE_LOCK(&buffer_lock);
	...
	RELEASE_LOCK(&buffer_lock);
```

The old `ret_common` path in `src/kernel/entry.S` scheduled a task on every
interrupt/system-call return whenever `current->need_resched` was set:

```asm
	movl need_resched(%ebx), %ecx
	testl %ecx, %ecx
	je 2f
	pushl %ebx
	call schedule
	popl %ebx
```

That schedule happened even when the interrupted frame was still in kernel
mode. The race was:

1. A task calls `get_buffer()` and acquires `buffer_lock`.
2. While that lock is still held, a PIT IRQ arrives.
3. The timer handler sets `current->need_resched`.
4. On the IRQ return, `ret_common` calls `schedule()` without checking
   whether the interrupted frame was user mode or kernel mode.
5. The task that owns `buffer_lock` is switched out while still holding the
   lock.
6. Another task enters `get_buffer()` and spins on `buffer_lock` forever.

This explains why the failure was intermittent: it depended on a timer IRQ
landing inside the small `get_buffer()` critical section.

## Fix

Change `ret_common` in `src/kernel/entry.S` so ordinary kernel-mode returns
do not reschedule. Rescheduling on kernel-mode return is only allowed for
the idle task, which is always safe to switch away from:

```asm
	GET_CURRENT(%ebx)
	testl $3, CS(%esp)
	jz 5f
	movl need_resched(%ebx), %ecx
	testl %ecx, %ecx
	je 2f
	pushl %ebx
	call schedule
	popl %ebx
2:
	...
5:
	cmpl $init_task_union, %ebx
	jne 3f
	movl need_resched(%ebx), %ecx
	testl %ecx, %ecx
	je 3f
	pushl %ebx
	call schedule
	popl %ebx
3:
	RESTORE_ALL
```

Normal tasks still reschedule when returning to user mode. Kernel code that
wants to yield does so at an explicit sleep/yield point where it is prepared
to drop any locks; it no longer gets unexpectedly switched out in the middle
of an arbitrary interrupt return.

## Verification

Rebuild the kernel and run the affected tests:

```sh
make CC=$PWD/util/mario-cc LD=$PWD/util/mario-ld -C src -j2
.venv/bin/python tests/lit_shell_test.py --test heredoc.test tests/shell
.venv/bin/python tests/lit_shell_test.py --test oksh-0.test tests/shell
```

After the fix:

- `heredoc.test` passed 15/15 consecutive runs.
- `oksh-0.test` passed 15/15 consecutive runs.
- The full shell suite passed all 22 tests.
- The VGA suite passed all 4 tests.
- `git diff --check` reported no whitespace errors.
