# Kernel Stack Protector Plan

## Goal

Detect kernel stack-buffer overwrites as close to the overflowing function as
possible. A corrupted stack canary stops the kernel in the function that is
returning, instead of allowing the bad write to damage an unrelated allocator
structure later.

## Current State

- `Makefile` already exports `STACK_PROTECTOR` and the `DEBUG` preset can set
  it:
  - `DEBUG=1` defaults `STACK_PROTECTOR` to `1`.
  - `STACK_PROTECTOR=0` overrides the preset.
- `util/cc:57` currently adds `-fno-stack-protector` to the common compiler
  flags, so kernel C code is never instrumented.
- `src/kernel/start.S:118` calls `mario()`, the first C function.
- `src/Makefile` discovers all `*/*.[Sc]` files, so a new
  `src/kernel/stack_protector.c` is linked automatically.
- `printk()` and `hang()` are available from `src/include/printk.h`.

## How It Works

With GCC's stack protector enabled, an instrumented function:

1. stores a copy of `__stack_chk_guard` immediately before its saved return
   address,
2. runs the function body,
3. reloads the real `__stack_chk_guard`,
4. compares it with the saved copy,
5. calls `__stack_chk_fail()` if they differ.

The kernel is freestanding, so it must provide both symbols.

## Configuration

Use one make variable for the feature and no special CFLAGS in `src/Makefile`:

| Variable | Meaning |
| --- | --- |
| `STACK_PROTECTOR=1` | Instrument kernel C objects with `-fstack-protector-strong` |
| `STACK_PROTECTOR=0` | Keep the current `-fno-stack-protector` behavior |

`OPTIMIZE` remains a code-generation option and must not control this check.

## Step 1: Teach `util/cc` to select the flag

Move the unconditional `-fno-stack-protector` out of the common `mario-*`
flags and apply it per wrapper:

- For `mario-cc`:
  - if `STACK_PROTECTOR=1`, append `-fstack-protector-strong`;
  - otherwise append `-fno-stack-protector`.
- Keep `-fno-stack-protector` for `mario-musl-cc`.
  This plan only protects the kernel.

Pseudocode in `util/cc`:

```python
stack_protector = os.environ.get("STACK_PROTECTOR") == "1"

case "mario-cc":
    flags.append("-fstack-protector-strong" if stack_protector
                 else "-fno-stack-protector")
```

`-fstack-protector-strong` is preferred over `-fstack-protector`: it covers
arrays, address-taken locals, and functions with alloca-like patterns, without
instrumenting every function in the kernel.

Acceptance:

```sh
make -n STACK_PROTECTOR=1 kernel | grep -- '-fstack-protector-strong'
make -n STACK_PROTECTOR=0 kernel | grep -- '-fno-stack-protector'
```

## Step 2: Add the guard and failure handler

Add `src/kernel/stack_protector.c`:

```c
#include <types.h>
#include <printk.h>

uint32_t __stack_chk_guard;

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void)
{
    pr_emerg("kernel stack smashing detected\n");
    hang();
}
```

`uint32_t` matches the four-byte canary GCC uses for i386. The failure handler
must be `noreturn` and `no_stack_protector` so it cannot recurse.

Acceptance:

- `src/kernel/stack_protector.o` appears in the kernel link.
- `nm src/kernel/kernel.exe | grep __stack_chk` shows both symbols.

## Step 3: Initialize the guard before protected C code

`start.S` calls `mario()` before any other C code. If `mario()` is instrumented,
its prologue reads `__stack_chk_guard` before the kernel can initialize it.

Recommended initial approach:

1. Mark `mario()` with `__attribute__((no_stack_protector))`.
2. Declare `extern uint32_t __stack_chk_guard;`.
3. Set the guard at the very top of `mario()`, before the first call to any
   other C function:

```c
__attribute__((no_stack_protector))
void mario(struct multiboot_info *m)
{
    __stack_chk_guard = 0x6d617269;
    percpu_init();
    printk_init(m);
    ...
}
```

Use a fixed nonzero seed for the first implementation. A later refinement can
mix in `rdtsc` or multiboot-provided entropy before assigning the guard.

Alternative: initialize `__stack_chk_guard` in `start.S` immediately before
`call mario`. That allows `mario()` itself to stay instrumented, but the seed
is then less convenient to randomize from C.

Acceptance:

- The kernel boots normally with `STACK_PROTECTOR=1`.
- The guard is nonzero before `percpu_init()` runs.

## Step 4: Validate the failure path

For a manual check, temporarily add a `noinline` test function:

```c
__attribute__((noinline))
static void stack_protector_selftest(void)
{
    char buf[16];

    buf[32] = 0;
}
```

Call it from `mario()` after the guard is initialized. The kernel should print
`kernel stack smashing detected` and halt. Remove the test call before
committing, or keep it behind a separate `STACK_PROTECTOR_SELFTEST` make
variable if repeatable boot-time validation is desired.

Acceptance:

- The deliberate overwrite stops in `__stack_chk_fail()`.
- The same boot succeeds after removing the test call.

## Step 5: Regression checks

Run both configurations:

```sh
make kernel STACK_PROTECTOR=0
make kernel STACK_PROTECTOR=1
make test SMP=1
make test SMP=4
```

The default build has `DEBUG=1`, so `make kernel` should also use the stack
protector once this feature is implemented.

Acceptance:

- Shell/TTY tests pass with the feature enabled.
- No link error is reported for `__stack_chk_guard` or `__stack_chk_fail`.
- Hot-path regressions are measured before enabling stronger instrumentation
  in non-debug builds.

## Later Refinements

- Replace the fixed seed with `rdtsc`-mixed entropy.
- Move to a per-CPU `__stack_chk_guard` once SMP behavior is validated.
- Evaluate enabling protection for the custom libc or musl user space.
- Add a permanent debug-only self-test behind its own make variable.

## Suggested Commits

1. `build: add STACK_PROTECTOR flag selection to mario-cc`
2. `kernel: add stack protector guard and failure handler`
3. `kernel: initialize stack guard before protected C code`

## Out of Scope

- Protecting assembly code or the boot trampoline.
- User-space stack protector for custom libc/musl.
- Automatic fuzzing of stack-overflow sites.
