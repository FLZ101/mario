# Memory Corruption Prevention Plan

## Purpose

Prevent the two recurring memory-corruption patterns observed in this kernel:

1. A local metadata object is read before every field that controls a later
   write has been initialized. The bad write happens far from the allocator,
   but the visible failure is a corrupted `struct page` or buddy free list.
   Example: `0201c63`.
2. A virtual/physical address conversion is wrong for the value being passed,
   so a refcount/free operation lands outside the intended `struct page`.
   Example: the page-table release bug documented in
   `doc/PYTHON_SUBPROCESS_SEGFAULT_DEBUG.md`.

The plan is intentionally layered:

1. make invalid initial state impossible,
2. detect invalid usage at allocator boundaries,
3. let the compiler find the bug class statically,
4. add focused regression and stress tests,
5. keep the debugging recipe repeatable for humans.

## Current State

Already landed:

- `0201c63 fat: initialize LFN state before directory lookup`
  - `fat_lookup()` and `fat_readdir()` zero `struct fat_lfn` before scanning.
  - Fix location: `src/fs/fat/dir.c:73`, `src/fs/fat/dir.c:1238`,
    `src/fs/fat/dir.c:1331`.
- `9653dfa fs/fat: initialize index in fat_delete_entry_at`
  - The later `index == 0` check is now preceded by
    `fat_dir_cluster_index()` initialization instead of reading an
    uninitialized local.
  - Fix location: `src/fs/fat/dir.c:885`.
- `601a48a mm: add page allocator invariant assertions`
  - Free-list linkage, PFN bounds, order bounds, and buddy alignment checks.
  - Fix location: `src/mm/page_alloc.c`.
- `51fe07d build: add clang-tidy hook and fix findings`
  - Added `.clang-tidy` with the selected memory-safety checks.
  - Added a local `clang-tidy` pre-commit hook scoped to `^src/.*\.c$`.
  - Added top-level `venv`, `precommit-install`, `precommit`, and
    `precommit-all` targets.
  - Fixed the initial findings: zero-initialized `blkdev`/AHCI/filemap
    objects, `__builtin_offsetof` for `dirent` name offsets, and intentional
    `sizeof` diagnostics in `src/fs/poll.c`.
- `8a3ba56 Add fatal stack diagnostics and filesystem fixes`
  - `hang()` now dumps the trapping CPU's registers and a bounded EBP
    backtrace to COM1 before halting, so a fault is reported at its source
    instead of surfacing later as allocator corruption.
  - Fix location: `src/kernel/panic.c`, `src/include/panic.h`.
  - Design notes: `doc/STACK_DUMP_PLAN.md`.
- Related concurrency fixes that remove intermittent corruption triggers:
  - `7cd1ea0 smp: keep wakeups on the task's current CPU`
  - `b33e481 ahci: recover from missed shared completion IRQs`
- Human debugging notes already exist:
  - `doc/VFAT_LFN_STACK_CORRUPTION_FIX.md`
  - `doc/PYTHON_SUBPROCESS_SEGFAULT_DEBUG.md`
  - `doc/SHARED_DATA_STRUCTURES_AUDIT.md`

Implemented by this rollout, in order:

- `0a6d20c build: restore test-all target`
- `19bd8e3 fat: make LFN fragment append self-initializing`
- `323bb39 fs: initialize stateful lookup/scan objects`
- `45b27d5 mm: add kzalloc() and use it for metadata`
- `59a39c7 build: add AUTO_VAR_INIT automatic local initialization`
- `b1be492 build: finish runtime-check plumbing`
- `3266779 mm: add KMALLOC_DEBUG header, redzone, and use-after-free check`
- `f347f9d mm: add PAGE_ALLOC_DEBUG owner/poison check`
- `451c8f9 kernel: add STACK_PROTECTOR support`
- `4989de1 kernel: keep stack canary consistent across CPUs`
- `1105b30 kernel: add UBSAN instrumentation and freestanding handlers`
- `fe86c9c build: add memory-corruption warning flags`
- `21612d4 build: add podman-aware make analyze-kernel`
- `f9ad823 test: add deliberate corruption self-tests`
- `6bf686f test: add VFAT LFN stress and SMP allocator stress`

Existing build characteristics that this plan should preserve or exploit:

- `src/Makefile` already builds with `-Wall -Werror`.
- `OPTIMIZE ?= -O0` controls code generation only. Set `OPTIMIZE=-O3`
  explicitly for optimized builds.
- `DEBUG ?= 1` is the aggregate runtime-check preset. It enables all runtime
  checks by default; specific check variables override it.
- Kernel builds include `-ggdb` and `-fno-omit-frame-pointer`.
- `util/cc` currently adds `-fno-stack-protector` to the `mario-*` wrappers.
- `compile_commands.json` is generated at the repository root.
- `.pre-commit-config.yaml` already runs `black` plus the local
  `clang-tidy` hook.
- `make precommit` runs hooks on staged files; `make precommit-all` runs them
  over the whole tree.
- `make test` builds the VBE image and runs shell/tty/gui tests for one
  `SMP` value; CI repeats it for `SMP=1/4/16` with `LIT_JOBS=1`.

## Build Configuration Model

`DEBUG` is an aggregate runtime-check preset, not an optimization switch. The
top-level `Makefile` accepts `DEBUG`/`OPTIMIZE` and forwards them to
`Makefile.build.mk`, where the per-check variables are computed. By default
`DEBUG=1` enables all runtime checks; `DEBUG=0` disables them unless a
specific check overrides it:

```make
OPTIMIZE ?= -O0

DEBUG ?= 1
DEBUG_TRUE := $(filter 1 true TRUE True,$(DEBUG))
ifeq ($(DEBUG_TRUE),)
KMALLOC_DEBUG ?= 0
PAGE_ALLOC_DEBUG ?= 0
STACK_PROTECTOR ?= 0
AUTO_VAR_INIT ?= 0
else
KMALLOC_DEBUG ?= 1
PAGE_ALLOC_DEBUG ?= 1
STACK_PROTECTOR ?= 1
AUTO_VAR_INIT ?= 1
endif
```

Precedence, highest first:

1. Command-line specific option, for example
   `make DEBUG=0 KMALLOC_DEBUG=1`.
2. Environment specific option (GNU make treats an exported variable as
   already defined, so `?=` does not override it).
3. `DEBUG` aggregate preset defaults.
4. Makefile fallback defaults.

Because `DEBUG` sets the specific options with `?=`, an explicit specific
option is never overwritten by `DEBUG`.

Current status:

- `AUTO_VAR_INIT`, `KMALLOC_DEBUG`, `PAGE_ALLOC_DEBUG`, `STACK_PROTECTOR`,
  `UBSAN`, and `ANALYZE` are all wired through `Makefile.build.mk` and
  `src/Makefile`, and their kernel consumers are implemented.
- The self-test targets are available through both `Makefile` and
  `Makefile.build.mk`:
  `selftest-kmalloc`, `selftest-page`, `selftest-stack`, `selftest-ubsan`.
- `DEBUG=1` enables all default runtime checks; `DEBUG=0` disables them.
  `UBSAN` and `ANALYZE` remain opt-in and default to `0`.

| Make variable | C macro / effect | Consumer | Phase |
| --- | --- | --- | --- |
| `KMALLOC_DEBUG` | `CONFIG_KMALLOC_DEBUG` | `src/mm/kmalloc.c` | 2.1 |
| `PAGE_ALLOC_DEBUG` | `CONFIG_PAGE_ALLOC_DEBUG` | `src/mm/page_alloc.c` | 2.2 |
| `STACK_PROTECTOR` | compiler flag selection | `util/cc` | 2.3 |
| `AUTO_VAR_INIT` | `-ftrivial-auto-var-init=pattern` | `src/Makefile` | 1.4 |
| `UBSAN` | sanitizer flags + `CONFIG_UBSAN` | kernel runtime handlers | 2.4 |
| `ANALYZE` | `-fanalyzer` flag | `src/Makefile` | 3.2 |

## Step-by-Step Execution Plan

Run the steps in order. Each step is a separate, buildable commit with its own
acceptance checks. The detailed instructions follow in the referenced phase.

0. **Baseline** — build and run the focused tests before changing code.
1. **1.1** — make the FAT LFN accumulator self-initializing.
2. **1.2** — audit and zero-initialize stateful local objects.
3. **1.3** — add `kzalloc()` and convert metadata allocations.
4. **1.4** — enable automatic initialization of automatic variables.
5. **2.0** — finish the runtime-check build plumbing.
6. **2.1** — add the `KMALLOC_DEBUG` header, redzone, and use-after-free check.
7. **2.2** — add the `PAGE_ALLOC_DEBUG` state/owner/poison check.
8. **2.3** — add kernel stack protector support.
9. **2.4** — add UBSan instrumentation and handlers.
10. **3.1** — enable the targeted memory-corruption warning flags.
11. **3.2** — add the podman-aware `analyze-kernel` target.
12. **4.1** — add deliberate corruption self-tests.
13. **4.2–4.3** — add VFAT LFN and allocator regression/stress tests.
14. **5** — record the debugging checklist and run the rollout gate.

## Rules

- Fix the initialization defect at the source, not only at the allocator.
- Every runtime check must have its own make variable and matching C macro,
  independent of `OPTIMIZE`.
- `DEBUG=1` enables all runtime checks by default; `DEBUG=0` disables them.
  Specific options always have higher precedence, for example
  `make DEBUG=0 KMALLOC_DEBUG=1`.
- Runtime checks are enabled by default. Use `DEBUG=0` or disable individual
  options for release-style builds; keep non-debug builds free of runtime-check
  overhead unless the overhead is proven negligible.
- Keep `-Werror`; do not add a warning flag until the kernel builds cleanly
  with it.
- Each phase must be independently buildable, testable, and committable.

## Baseline Validation

Run before starting a phase:

```sh
make kernel
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 tests/shell/vfat.write.test
.venv/bin/python tests/lit_shell_test.py --smp 1 --jobs 1 tests/shell/python-1.test
```

After allocator changes, also run:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 tests/shell/vfat.stress.test
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 tests/shell/ext2.stress.test
```

## Phase 1: Initialization Hardening

Highest leverage, lowest risk. This directly removes the root-cause pattern.

### 1.1 Make the LFN accumulator self-protecting

Current `fat_lfn_reset()` zeroes `struct fat_lfn`, and callers are expected to
invoke it before a new sequence. That is correct but relies on every future
caller remembering the reset.

Change `fat_lfn_add()` in `src/fs/fat/dir.c` so the first LFN fragment is
always preceded by a reset:

```c
if (!lfn->have_last) {
    fat_lfn_reset(lfn);
    if (!(entry[0] & 0x40))
        return -EINVAL;
    lfn->have_last = 1;
    lfn->next_seq = seq - 1;
    lfn->checksum = checksum;
}
```

Also add debug assertions at the top of `fat_lfn_finish()`:

```c
assert(lfn->len <= (int)sizeof(lfn->name) - 1);
assert(lfn->next_seq >= 0 && lfn->next_seq <= 20);
```

Keep the explicit `fat_lfn_reset()` calls at call sites; the self-reset is a
second line of defense, not a replacement.

Acceptance:

```sh
make kernel
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 tests/shell/vfat.write.test
```

### 1.2 Audit stateful local objects

Search for local structs that have a length/index field or an internal buffer
and are conditionally updated before use:

```sh
rg -n "struct .*\{|char .*\[|int len|unsigned long len|size_t len" src --glob '*.[ch]'
```

Review at least:

- `src/fs/fat/dir.c`
- `src/fs/ext2/dir.c`
- `src/fs/mariofs/dir.c`
- `src/mm/filemap.c`
- `src/fs/proc.c`
- `src/fs/sysfs.c`
- `src/fs/debugfs.c`
- `src/kernel/smp.c`
- `src/fs/socket.c`

For every reviewed object, ensure one of:

```c
struct foo foo = {0};
```

or an explicit reset helper:

```c
memset(&foo, 0, sizeof(foo));
```

happens before the first read or before the first write through a computed
length/index.

Acceptance:

- No object is read before the fields controlling a later write are set.
- Full shell tests pass for both `SMP=1` and `SMP=8`.

### 1.3 Add `kzalloc()`

`kmalloc()` returns uninitialized memory. Add a zeroing variant so metadata
callers do not have to remember every field:

- Add to `src/include/mm/kmalloc.h`:

  ```c
  void *kzalloc(size_t size);
  ```

- Implement in `src/mm/kmalloc.c` as:

  ```c
  void *kzalloc(size_t size)
  {
      void *ptr = kmalloc(size);
      if (ptr)
          memset(ptr, 0, size);
      return ptr;
  }
  ```

Convert allocations that represent metadata or otherwise should start zeroed.
Reasonable candidates include:

- `struct mm_struct`, `struct fs_struct`, `struct files_struct` in
  `src/kernel/fork.c`
- `struct vm_area_struct` in `src/mm/mm.c`
- `struct unix_socket` and `struct unix_socket_pair` in `src/fs/socket.c`

Do not blanket-convert large data buffers that are immediately and completely
overwritten; the extra `memset()` would only waste cycles.

Acceptance:

```sh
make kernel
.venv/bin/python tests/lit_shell_test.py tests/shell
```

### 1.4 Enable automatic variable initialization

The manual audit in 1.2 fixes known sites, but future uninitialized locals
remain the same bug class. Make the compiler zero or pattern-initialize every
automatic variable unless it is explicitly initialized:

- Add `AUTO_VAR_INIT ?= 1` to `Makefile.build.mk` and preset it from `DEBUG`
  exactly like `KMALLOC_DEBUG`.
- Pass `AUTO_VAR_INIT=$(AUTO_VAR_INIT)` from the top-level `kernel` target to
  `Makefile.build.mk`, and from there to the kernel submake.
- In `src/Makefile`, when `AUTO_VAR_INIT=1`, append
  `-ftrivial-auto-var-init=pattern` to `CFLAGS`; when it is `0`, do not add
  the flag.
- Confirm the installed GCC/Clang supports the flag before enabling it in CI.
  If the oldest supported build image lacks it, keep the step behind the
  variable and document the unsupported image.

Use `pattern` first because it exposes accidental dependence on a specific
zeroed pattern; fall back to `zero` only if `pattern` produces too much noise
or an unsupported toolchain.

Acceptance:

```sh
make kernel AUTO_VAR_INIT=1
make kernel AUTO_VAR_INIT=0
make SMP=1 LIT_JOBS=1 test
```

The VFAT LFN initialization bug class is caught by the compiler even if a
future caller forgets to reset its local metadata object.

## Phase 2: Runtime Corruption Detection

These changes move detection closer to the invalid write and to free time.

### 2.0 Finish the runtime-check build plumbing

The preset variables already exist in `Makefile.build.mk`, but the consumers
do not yet receive every value they need.

- Keep `DEBUG` as the aggregate preset and preserve the `?=` override rule.
- Forward each specific check variable from the top-level `kernel` target to
  `Makefile.build.mk` so command-line overrides survive `MAKEOVERRIDES :=`.
- Export `STACK_PROTECTOR` so `util/cc` can select the stack-protector flag.
- Wire `UBSAN` through the top-level `Makefile`, `Makefile.build.mk`, and the
  kernel submake before implementing the handlers in 2.4.
- Pass `AUTO_VAR_INIT` to `src/Makefile` from 1.4.
- Add each `-DCONFIG_*` flag in `src/Makefile` from the corresponding make
  variable, not from `OPTIMIZE` or `DEBUG` directly.

Acceptance:

```sh
make -n DEBUG=1 KMALLOC_DEBUG=1 STACK_PROTECTOR=1 kernel |
  grep -E -- '-DCONFIG_KMALLOC_DEBUG|-fstack-protector'
make -n DEBUG=0 KMALLOC_DEBUG=0 STACK_PROTECTOR=0 kernel |
  grep -E -- '-fno-stack-protector'
```

Gate convention:

- Make variable `KMALLOC_DEBUG=1` adds `-DCONFIG_KMALLOC_DEBUG`.
- Make variable `PAGE_ALLOC_DEBUG=1` adds `-DCONFIG_PAGE_ALLOC_DEBUG`.
- Make variable `STACK_PROTECTOR=1` selects the guarded `util/cc` path.
- Make variable `UBSAN=1` will add the selected `-fsanitize=` flags and
  `-DCONFIG_UBSAN` for the kernel runtime handlers.
- `DEBUG=1` presets the runtime-check variables to enabled; `DEBUG=0` presets
  them to disabled. Individual settings override `DEBUG`.
- A default `DEBUG=1` build enables every check. Disable individual checks
  explicitly, or use `DEBUG=0` to disable the whole group.
- The C code uses the `CONFIG_*` macro only; `OPTIMIZE` is not used as a
  runtime-check gate.

### 2.1 kmalloc debug header, redzone, and use-after-free check

The current block layout is packed:

```c
struct block_header {
    struct board_header *board;
    struct block_header *next;
} __attribute__((gcc_struct, packed));
```

`kmalloc()` returns `&block->next`, so the four bytes immediately before the
user pointer are the `next` field and the `board` pointer is the field before
that. `kfree()` recovers the full `struct block_header` from the pointer. The
allocator does not record the requested size, owner, or allocation/freed state.

Introduce a debug-only header behind `CONFIG_KMALLOC_DEBUG`, enabled by
`KMALLOC_DEBUG=1`:

- Add `magic`, `owner`, and the requested `size` to `struct block_header` when
  the macro is enabled.
- Keep the user pointer at the original `&block->next` offset in debug builds
  or adjust `kfree()` and the bucket geometry together; do not break the
  release layout.
- Adjust the `buckets[]` sizes in `src/mm/kmalloc.c` so the larger debug
  header is included; currently each bucket chooses
  `bucket->block_size - 4 >= size`.
- Store the requested allocation size so the allocator knows where the usable
  object ends.
- Place a redzone canary immediately after the usable object and validate it
  in `kfree()` to catch writes past the requested size.
- On free, poison the usable object and change `magic` to a distinct
  freed-magic value; reject double free in `kfree()`.
- Reject `NULL` and reject pointers whose magic is wrong.
- Keep release builds using the existing four-byte prefix.

Suggested fields:

```c
uint32_t magic;         /* allocated or freed magic */
int owner;              /* current->pid for the last owner */
size_t size;            /* requested usable size */
uint32_t redzone;       /* canary immediately after the usable object */
```

Acceptance:

- A deliberate double free, out-of-bounds write, or stale-pointer reuse that
  reaches `kfree()` stops with an assertion; freed-magic rejection and redzone
  validation cover the allocator-visible cases.
- Normal shell tests still pass with the default `KMALLOC_DEBUG=1` build.

### 2.2 Page allocator debug state

`struct page` is currently only:

```c
struct page {
    atomic_t count;
    struct list_head list;
};
```

Keep the invariants from `601a48a`. Then, under `CONFIG_PAGE_ALLOC_DEBUG`,
enabled by `PAGE_ALLOC_DEBUG=1`:

- Add a state field: `PG_FREE`, `PG_ALLOC`.
- Add an owner `pid` for the last allocation/free.
- Poison freed pages with a known pattern.
- In `free_pages()`, assert the page is currently `PG_ALLOC` before changing
  state.
- In `alloc_pages()`, assert the returned page is `PG_FREE`.

Because `mem_map` is sized from `sizeof(struct page)`, keep debug fields
explicitly conditional and make sure `page_alloc_init()` still accounts for
the full struct size.

Acceptance:

- Double free or freeing a live page stops in `free_pages()`.
- `vfat.write.test` and an SMP allocator stress test pass.

### 2.3 Kernel stack canary

The VFAT failure was a stack overwrite. A stack canary would have stopped at
the returning function rather than corrupting the page allocator later.

See `doc/STACK_PROTECTOR_PLAN.md` for the separate step-by-step plan.

### 2.4 Sanitizer instrumentation

Sanitizers are feasible, but only selectively because the kernel is
freestanding and cannot use the host libc sanitizer runtimes.

**Start with UndefinedBehaviorSanitizer (UBSan)**

UBSan does not require shadow memory. Add a `UBSAN ?= 0` make variable and,
when enabled, compile kernel C files with a focused set:

```make
UBSAN_FLAGS ?= -fsanitize=undefined
UBSAN_FLAGS += -fno-sanitize-recover=undefined
CFLAGS += $(UBSAN_FLAGS)
```

Start with the full undefined set if it builds cleanly; otherwise reduce it
to the highest-value subset, such as
`-fsanitize=bounds,object-size,pointer-overflow,shift`.

Provide minimal freestanding handlers, for example
`__ubsan_handle_type_mismatch_v1()`, `__ubsan_handle_out_of_bounds()`, and
`__ubsan_handle_pointer_overflow()`. Each handler prints the offending
site and calls `hang()`.

Acceptance:

- `make kernel UBSAN=1` builds and links.
- A deliberate out-of-bounds or pointer-overflow case stops in a UBSan
  handler.
- `make kernel UBSAN=0` has no sanitizer runtime references.

**Defer AddressSanitizer/KASAN**

ASan-style checking is possible later, but it is a much larger change:

- allocate a shadow memory region,
- instrument page allocator and accessors,
- handle early boot before paging/allocators are available,
- verify QEMU memory size and page-table mappings account for the shadow.

**Do not pursue now**

- MemorySanitizer: requires tracking uninitialized data across every
  allocator, file, and device path.
- ThreadSanitizer: requires a full runtime and scheduler integration.
- LeakSanitizer: less useful for a long-lived kernel heap.

## Phase 3: Static Analysis and Warning Flags

Compiler diagnostics are cheaper than runtime debugging when they fire.

### 3.1 Add targeted warning flags

In `src/Makefile`, introduce a separate variable and append it after existing
flags:

```make
WARN_FLAGS ?= -Wmaybe-uninitialized -Warray-bounds=2 -Wstringop-overflow=4
CFLAGS += $(WARN_FLAGS)
```

Build cleanly before enabling a new flag. Because `-Werror` is already set,
the flag should land in the same commit that fixes the warnings it finds.
These warnings are optimization-sensitive; evaluate them with an optimized
build (`OPTIMIZE=-O3`) rather than the default `OPTIMIZE=-O0` build.

Acceptance:

- `make kernel` succeeds with zero new warnings.
- The VFAT initialization defect is more likely to be caught by
  `-Wmaybe-uninitialized` or `-Wstringop-overflow`.

### 3.2 Add `make analyze-kernel`

Use GCC `-fanalyzer` as a slow, opt-in target:

- Add `ANALYZE ?= 0` to `src/Makefile`.
- When `ANALYZE=1`, append:

  ```make
  -fanalyzer -Wno-error=analyzer-too-complex
  ```

- Add an `analyze-kernel` target to `Makefile.build.mk` that uses the same
  `$(PODMAN_RUN)` invocation as `kernel`, but passes
  `OPTIMIZE=-O0 ANALYZE=1 DEBUG=0` and the mario CC/LD wrappers.
- Add the top-level passthrough:

  ```make
  analyze-kernel:
      make -f Makefile.build.mk DEBUG=0 OPTIMIZE=-O0 ANALYZE=1 analyze-kernel
  ```

Acceptance:

- `make analyze-kernel` completes and produces a reviewable warning list.
- It runs inside the recorded container when `.build-env` exists and directly
  on the host otherwise, matching `make kernel`.
- Each uninitialized-use or out-of-bounds finding is fixed or documented.

### 3.3 Clang-tidy git hook

This is already landed in `51fe07d`. The current configuration is:

```yaml
Checks: '-*,clang-analyzer-core.uninitialized.*,clang-analyzer-core.NullDereference,clang-analyzer-core.UndefinedBinaryOperatorResult,clang-analyzer-unix.Malloc,clang-analyzer-unix.MismatchedDeallocator,bugprone-sizeof-expression,bugprone-suspicious-memset-usage,bugprone-undefined-memory-manipulation'
WarningsAsErrors: '*'
HeaderFilterRegex: '^src/'
```

The local hook in `.pre-commit-config.yaml` invokes `clang-tidy` directly on
changed kernel C files:

```yaml
repos:
  - repo: local
    hooks:
      - id: clang-tidy
        name: clang-tidy
        entry: clang-tidy
        language: system
        args: ["-p", ".", "--quiet"]
        files: ^src/.*\.c$
        require_serial: true
```

It is installed and validated with:

```sh
.venv/bin/pre-commit install
.venv/bin/pre-commit run clang-tidy --all-files
```

Acceptance:

- `git commit` runs `clang-tidy` only on changed kernel sources.
- A commit introducing an uninitialized-use or obvious out-of-bounds pattern is
  blocked.
- `make SMP=1 LIT_JOBS=1 test` still passes after any manual fixes.

Remaining work:

- Add a CI/`make precommit-all` gate to the normal validation loop.
- After later phases introduce new patterns, broaden the check list only after
  the kernel is clean with the new set.

## Phase 4: Regression and Stress Tests

### 4.1 Deliberate corruption self-tests

Add boot-time or shell-runnable self-tests that prove each detector halts,
rather than only checking that normal workloads still pass. Gate them behind
the same runtime-check variables so release builds do not run them.

Self-test candidates:

- `KMALLOC_DEBUG=1`: double free, freeing a wrong magic, and writing past the
  requested size.
- `PAGE_ALLOC_DEBUG=1`: freeing a live page and double-freeing a page.
- `STACK_PROTECTOR=1`: overflow a small stack buffer and verify
  `__stack_chk_fail()` is reached.
- `UBSAN=1`: a deliberate `__builtin_unreachable()`-free out-of-bounds or
  pointer-overflow case that reaches a UBSan handler.

Acceptance:

- With the relevant check enabled, each self-test stops in the expected
  detector and prints a fatal diagnostic.
- With the check disabled, the self-test is not linked or is skipped.

### 4.2 VFAT LFN stress test

Add `tests/shell/vfat.lfn.stress.test`:

- create files with maximum-length LFN names,
- rename them across directories,
- read them back,
- unlink them,
- repeat the cycle several times.

Run with both SMP configurations:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 1 --jobs 1 tests/shell/vfat.lfn.stress.test
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 1 tests/shell/vfat.lfn.stress.test
```

Acceptance:

- The test fails before the hardening fixes and passes after them.

### 4.3 Allocator stress

Run the existing stress tests repeatedly and under SMP:

```sh
make SMP=1 LIT_JOBS=1 test
make SMP=8 LIT_JOBS=1 test
```

Targeted validation:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 4 tests/shell/vfat.stress.test tests/shell/ext2.stress.test
```

Acceptance:

- No page allocator assertions fire.
- No timeout regressions are attributable to the debug instrumentation.

## Phase 5: Documentation and Process

Keep `doc/VFAT_LFN_STACK_CORRUPTION_FIX.md` as the worked example and
`doc/STACK_DUMP_PLAN.md` as the fatal-site register/backtrace reference. The
following checklist is the repeatable process for a new corruption report.

### Memory Corruption Debugging Checklist

1. **Capture the first fault, not the allocator symptom.**
   - Run `make run` and keep the COM1 serial log.
   - `panic_hang()` in `src/kernel/panic.c` already prints the trapping CPU's
     registers and a bounded EBP backtrace before halting.
   - Treat a page/buddy assertion as a symptom; look above it for the first
     bad store or faulting return address.

2. **Audit initial state.**
   - `rg -n "struct .*\\{|char .*\\[|int len|unsigned long len|size_t len" src --glob '*.[ch]'`
   - Every stateful local object must be `= {0}`, `kzalloc()`, or explicitly
     `memset()` before the field that controls a later read/write is used.

3. **Run allocator invariants first.**
   - `make kernel DEBUG=1`
   - `make SMP=1 LIT_JOBS=1 test` and repeat the failing test.
   - The `KMALLOC_DEBUG` magic/redzone and `PAGE_ALLOC_DEBUG` owner/poison
     checks report misuse at the allocator boundary.

4. **Use GDB watchpoints on the overwritten value.**
   - Start `make debug`, then set a hardware watchpoint on the saved EBP/EIP
     or the corrupted `struct page` field seen in step 1.
   - The watchpoint's write is often the real defect, not the later `kfree`
     or buddy-merge that notices the damage.

5. **Enable the closest detector and let it stop at the source.**
   - Stack overwrite: `make kernel STACK_PROTECTOR=1`.
   - Uninitialized automatic variable:
     `make kernel AUTO_VAR_INIT=1` plus `OPTIMIZE=-O3` and the warning flags.
   - Undefined arithmetic/bounds/pointer misuse: `make kernel UBSAN=1`.
   - Static path bugs: `make analyze-kernel`.

6. **Prove the detector works.**
   - `make selftest-kmalloc`, `make selftest-page`,
     `make selftest-stack`, `make selftest-ubsan`
   - Each self-test must stop in its expected detector, not merely compile.

7. **Run the regression and rollout gates.**
   - `make test-all LIT_JOBS=1`
   - `make precommit-all`
   - `make SMP=1 LIT_JOBS=1 test`, `make SMP=4 LIT_JOBS=1 test`,
     `make SMP=16 LIT_JOBS=1 test`

### Make Variables and C Macros

The table in [Build Configuration Model](#build-configuration-model) records
every runtime-check and analysis variable. Quick reference:

| Make variable | Default | C macro / effect |
| --- | --- | --- |
| `AUTO_VAR_INIT` | follows `DEBUG` | `-ftrivial-auto-var-init=pattern` |
| `KMALLOC_DEBUG` | follows `DEBUG` | `CONFIG_KMALLOC_DEBUG` |
| `PAGE_ALLOC_DEBUG` | follows `DEBUG` | `CONFIG_PAGE_ALLOC_DEBUG` |
| `STACK_PROTECTOR` | follows `DEBUG` | guarded `util/cc` path |
| `UBSAN` | `0` | `CONFIG_UBSAN` + focused sanitizer flags |
| `ANALYZE` | `0` | `-fanalyzer` |

Self-test macros are internal to the self-test targets:
`KMALLOC_SELF_TEST`, `PAGE_ALLOC_SELF_TEST`,
`STACK_PROTECTOR_SELF_TEST`, and `UBSAN_SELF_TEST`.

## Suggested Commit Sequence

Already landed:

- `0201c63 fat: initialize LFN state before directory lookup`
- `9653dfa fs/fat: initialize index in fat_delete_entry_at`
- `601a48a mm: add page allocator invariant assertions`
- `51fe07d build: add clang-tidy hook and fix findings`
- `8a3ba56 Add fatal stack diagnostics and filesystem fixes`
- `7cd1ea0 smp: keep wakeups on the task's current CPU`
- `b33e481 ahci: recover from missed shared completion IRQs`

Remaining commits, each buildable and testable on its own: none. The rollout
commits are listed in [Current State](#current-state).

## Rollout Gate

Before enabling a runtime check in non-debug builds:

1. `make kernel` is warning-clean.
2. `make precommit-all` passes.
3. `make SMP=1 LIT_JOBS=1 test`, `make SMP=4 LIT_JOBS=1 test`, and
   `make SMP=16 LIT_JOBS=1 test` pass.
4. The CI matrix passes for the relevant build-image/flags combinations.
5. The measured overhead is documented and acceptable.
