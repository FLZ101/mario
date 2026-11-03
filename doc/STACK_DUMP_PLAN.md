# Fatal Halt Stack-Dump Plan

## Purpose

When the kernel executes `hang(...)`, print one reliable diagnostic to QEMU
serial output before entering the permanent `cli; hlt` loop. The diagnostic
must identify the call site, preserve the CPU state observed at that site, and
include a bounded kernel backtrace that can be resolved with `addr2line`.

This is a kernel-only diagnostic. It does not depend on QMP, test-runner CPU
timeouts, a QEMU migration state, the framebuffer, or the tty subsystem.

## Current Constraints

- `hang(...)` currently calls `printk()` and then executes `cli; hlt` in
  `src/include/printk.h`.
- `printk()` writes to the foreground framebuffer console, while QEMU test
  runners consume COM1 through `-serial stdio` or a serial socket.
- `uart_poll_write()` already provides polling output in
  `src/fs/char/serial.c`. Polling does not require interrupts, but its current
  unbounded wait is unsuitable for a fatal path.
- Kernel C code is built with `-fno-omit-frame-pointer`, so ordinary C frames
  form an i386 EBP chain.
- Every active task records its kernel-stack top in `current->thread.esp0`.
  This also holds for AP idle stacks.
- `struct trap_frame` already captures the register state for exceptions.

## Non-Goals for the First Version

- Do not redirect `/dev/console` or all normal `printk()` output to serial.
  Normal console behavior remains unchanged.
- Do not unwind user stacks.
- Do not stop other CPUs with a new IPI in the first version. The diagnostic
  must be correct and non-interleaved before adding cross-CPU stopping.
- Do not add a permanent userspace interface that intentionally halts the
  kernel.

## Design Decisions

### Dedicated Fatal Writer

Create `src/kernel/panic.c` and `src/include/panic.h`.

The module owns a minimal COM1 writer with these properties:

- It writes directly to COM1, never through tty, framebuffer, `printk()`, or
  a console lock.
- It performs a bounded poll for the UART transmit-holding-register-empty bit.
  If the UART does not become ready within the fixed poll budget, it drops that
  byte and continues toward the halt rather than hanging in diagnostics.
- It supports only the formatting required by the report: literals, decimal
  and fixed-width hexadecimal integers, and bounded strings. It must not
  allocate memory.
- It uses a statically allocated message buffer, protected by the panic-owner
  guard, if formatted `hang()` messages need a `va_list` formatter.

Expose one early serial initialiser, separate from tty registration:

```c
void panic_serial_init(void);
```

Call it in `mario()` immediately after `percpu_init()` and before
`printk_init()`. It configures COM1 without creating a tty. Normal
`serial_init()` may configure COM1 again later. This establishes serial output
for fatal failures during early memory, interrupt, block, or filesystem setup.

### Single Panic Owner

Use a global `volatile unsigned int panic_owner` acquired with x86 `xchg`.

1. The first CPU to acquire it is the reporting CPU.
2. A recursive fatal error on that CPU, or a concurrent fatal error on another
   CPU, skips formatting and enters its own `cli; hlt` loop immediately.
3. The reporting CPU disables interrupts, emits the complete report, and then
   halts.

This prevents interleaved serial data. It does not freeze healthy CPUs; that
requires a later, explicit panic-stop IPI design.

## Data Model

Use a context supplied by the `hang()` call site:

```c
struct panic_regs {
	unsigned long eax;
	unsigned long ebx;
	unsigned long ecx;
	unsigned long edx;
	unsigned long esi;
	unsigned long edi;
	unsigned long ebp;
	unsigned long esp;
	unsigned long eip;
	unsigned long eflags;
};

struct panic_site {
	const char *file;
	const char *function;
	unsigned int line;
};
```

`panic_hang()` receives the register snapshot, source site, and formatted
reason. It is marked non-returning.

```c
void panic_hang(const struct panic_regs *regs,
		const struct panic_site *site,
		const char *format, ...) __attribute__((noreturn));
```

## `hang()` Capture Contract

Replace the existing `hang(...)` macro with a wrapper that:

1. Allocates `struct panic_regs` and `struct panic_site` in the caller.
2. Uses one `asm volatile` block with memory outputs and a `memory` clobber to
   capture the general registers, EBP, ESP, and EFLAGS before any C call.
3. Captures a local code label as EIP. The reported EIP therefore resolves to
   the function and source region containing the `hang()` invocation.
4. Calls `panic_hang()` with `__FILE__`, `__func__`, and `__LINE__`.

The capture operation must be a macro, not an out-of-line helper: a function
call would already have changed caller-visible registers and the stack.

The original EFLAGS must be sampled before `panic_hang()` clears interrupts.
The dump can then distinguish a fatal `cli; hlt` from normal idle code that
uses `sti; hlt`.

## Backtrace Algorithm

The walker follows the i386 frame layout:

```c
struct stack_frame {
	unsigned long previous_ebp;
	unsigned long return_address;
};
```

### Establish Bounds

For an explicit `hang()`:

1. Read the captured ESP.
2. Read `current->thread.esp0` only after confirming `current` is available.
3. Set `stack_top = current->thread.esp0` and
   `stack_bottom = stack_top - KSTACK_SIZE`.
4. Reject unwinding if arithmetic wraps, if `stack_top - stack_bottom` is not
   `KSTACK_SIZE`, or if captured ESP is outside those bounds.

Early failures before per-CPU current-task setup still print the reason and
registers, but report `stack: unavailable (early boot)` rather than attempting
to dereference a guessed stack address.

### Walk Safely

- Start at the captured EBP.
- Print at most 32 return addresses.
- Before reading a frame, require 4-byte alignment and require the complete
  two-word frame to lie in `[stack_bottom, stack_top)`.
- Require `previous_ebp > ebp`; i386 stacks grow downward, so valid older
  frame pointers progress upward toward `stack_top`.
- Stop if the next EBP does not fit a complete frame.
- Optionally reject return addresses outside the kernel text range. Export
  `_etext` from `src/kernel.ld` alongside the existing `_text` symbol so this
  range is explicit.

The walker prints raw virtual addresses only. On the host, resolve them with:

```sh
addr2line -e src/kernel/kernel.exe -fip 0x8010....
```

## Exception Integration

Keep normal exception semantics unchanged in the first patch. Add a separate
serial trap-dump helper that can be called for a fatal kernel exception later.

For that follow-up:

- Print the existing `struct trap_frame` fields directly; its EIP and general
  registers are the interrupted state, not registers modified by C handlers.
- Only walk `tr->ebp` when `userland(tr)` is false and the EBP lies in the
  active kernel stack bounds.
- For user exceptions, report the trap frame but never dereference user EBP.
- Decide separately whether a kernel exception should remain task-local or
  invoke `panic_hang()`. This plan does not change current signal and
  `do_exit()` behavior.

## Report Format

Keep the output stable and machine-searchable:

```text
=== MARIO FATAL HANG ===
cpu=0 pid=12 task=example
reason: Assertion 'node != NULL' failed
site: src/fs/fat/dir.c:123 fat_lookup
regs: eip=80101234 eflags=00000002 esp=...
regs: eax=... ebx=... ecx=... edx=...
regs: esi=... edi=... ebp=...
stack:
  #00 8010abcd
  #01 8010cdef
  #02 8010fedc
halted
```

Emit `stack: unavailable (...)` or `stack: stopped (...)` with a specific
reason whenever validation rejects the frame chain.

## Implementation Sequence

1. Add the panic header/module and bounded COM1 output primitives.
2. Add `panic_serial_init()` and call it immediately after `percpu_init()`.
3. Add the panic-owner guard and a reason-only `panic_hang()` path; verify it
   halts after writing to QEMU serial.
4. Add the call-site register-capture macro and route `hang(...)` through it.
5. Add bounded EBP walking using captured ESP/EBP and `thread.esp0` bounds.
6. Add `_etext` and optional return-address text-range validation.
7. Add fatal kernel-trap reporting only after the explicit-hang path is proven
   stable.
8. Consider a separate panic-stop IPI feature after the one-CPU report is
   reliable on SMP.

## Validation Plan

Use a temporary local development-only call chain ending in
`hang("stack dump self-test")`; do not commit an intentional production
runtime halt trigger.

Validate in this order:

1. `make kernel` succeeds.
2. Boot the temporary trigger with QEMU `-serial stdio`; the complete marker,
   reason, registers, and stack arrive before the CPU halts.
3. Run `addr2line` on every emitted return address and confirm the expected
   layered test functions appear in order.
4. Trigger an `assert()` and confirm it uses the identical report path.
5. Corrupt or truncate the self-test frame chain and verify the walker stops
   cleanly rather than faulting or looping.
6. Boot with two CPUs and verify exactly one report is emitted even if two CPUs
   reach the test panic path.
7. Boot normally and verify ordinary `sti; hlt` idle remains silent.
