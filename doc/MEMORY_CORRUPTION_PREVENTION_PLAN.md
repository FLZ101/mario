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
- `601a48a mm: add page allocator invariant assertions`
  - Free-list linkage, PFN bounds, order bounds, and buddy alignment checks.
  - Fix location: `src/mm/page_alloc.c`.
- `51fe07d build: add clang-tidy hook and fix findings`
  - Added `.clang-tidy` with the selected memory-safety checks.
  - Added a local `clang-tidy` pre-commit hook scoped to `^src/.*\.c$`.
  - Added top-level `precommit-install`, `precommit`, `precommit-all`, and
    `setup` targets.
  - Fixed the initial findings: zero-initialized `blkdev`/AHCI/filemap
    objects, `__builtin_offsetof` for `dirent` name offsets, and intentional
    `sizeof` diagnostics in `src/fs/poll.c`.
- Related concurrency fixes that remove intermittent corruption triggers:
  - `7cd1ea0 smp: keep wakeups on the task's current CPU`
  - `b33e481 ahci: recover from missed shared completion IRQs`
- Human debugging notes already exist:
  - `doc/VFAT_LFN_STACK_CORRUPTION_FIX.md`
  - `doc/PYTHON_SUBPROCESS_SEGFAULT_DEBUG.md`
  - `doc/SHARED_DATA_STRUCTURES_AUDIT.md`

Existing build characteristics that this plan should preserve or exploit:

- `src/Makefile` already builds with `-Wall -Werror`.
- `OPTIMIZE ?= -O0` controls code generation only. Set `OPTIMIZE=-O3`
  explicitly for optimized builds.
- `DEBUG ?= 1` is the aggregate runtime-check preset. It enables all runtime
  checks by default; specific check variables override it.
- Kernel builds include `-ggdb` and `-fno-omit-frame-pointer`.
- `util/cc` currently adds `-fno-stack-protector` to the kernel wrapper.
- `compile_commands.json` is generated at the repository root.
- `.pre-commit-config.yaml` already runs `black` plus the local
  `clang-tidy` hook.
- `make precommit` runs hooks on staged files; `make precommit-all` runs them
  over the whole tree.
- `make test-all` runs shell/tty/gui tests for `SMP=1/2/4/8/16`.

## Build Configuration Model

`DEBUG` is an aggregate runtime-check preset, not an optimization switch. By
default `DEBUG=1` enables all runtime checks; `DEBUG=0` disables them unless
a specific check overrides it:

```make
OPTIMIZE ?= -O0

DEBUG ?= 1
ifeq ($(DEBUG),1)
KMALLOC_DEBUG ?= 1
PAGE_ALLOC_DEBUG ?= 1
STACK_PROTECTOR ?= 1
else
KMALLOC_DEBUG ?= 0
PAGE_ALLOC_DEBUG ?= 0
STACK_PROTECTOR ?= 0
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

## Next Action

Start with Phase 1.1. It directly removes the LFN initialization defect with
the lowest risk, then proceed through 1.2 and 1.3 before adding runtime
canaries in Phase 2.

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

## Phase 2: Runtime Corruption Detection

These changes move detection closer to the invalid write and to free time.

Gate convention:

- Make variable `KMALLOC_DEBUG=1` adds `-DCONFIG_KMALLOC_DEBUG`.
- Make variable `PAGE_ALLOC_DEBUG=1` adds `-DCONFIG_PAGE_ALLOC_DEBUG`.
- Make variable `STACK_PROTECTOR=1` selects the guarded `util/cc` path.
- Make variable `UBSAN=1` will add the selected `-fsanitize=` flags and
  `-DCONFIG_UBSAN` for the kernel runtime handlers.
- `DEBUG=1` presets all three to enabled; `DEBUG=0` presets them to disabled.
  Individual settings override `DEBUG`.
- A default `DEBUG=1` build enables every check. Disable individual checks
  explicitly, or use `DEBUG=0` to disable the whole group.
- The C code uses the `CONFIG_*` macro only; `OPTIMIZE` is not used as a
  runtime-check gate.

### 2.1 kmalloc debug header, canary, and double-free check

The current block layout is packed:

```c
struct block_header {
    struct board_header *board;
    struct block_header *next;
} __attribute__((gcc_struct, packed));
```

`kmalloc()` returns `&block->next`, and `kfree()` subtracts four bytes to get
the header. The allocator does not record the requested size, owner, or
allocation/freed state.

Introduce a debug-only header behind `CONFIG_KMALLOC_DEBUG`, enabled by
`KMALLOC_DEBUG=1`:

- Add a magic word and owner tag to `struct block_header` when the macro is
  enabled.
- Adjust the `buckets[]` sizes in `src/mm/kmalloc.c` so the larger debug
  header is included; currently each bucket chooses
  `bucket->block_size - 4 >= size`.
- Validate magic and allocated/freed state in `kfree()`.
- Reject `NULL` and reject pointers whose magic is wrong.
- Keep release builds using the existing four-byte header.

Suggested fields:

```c
uint32_t magic;         /* allocated or freed magic */
int owner;              /* current->pid for the last owner */
```

Acceptance:

- A deliberate double free stops in `kfree()` with an assertion.
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

- Add a top-level target:

  ```make
  analyze-kernel:
      $(MAKE) -C src OPTIMIZE=-O0 ANALYZE=1
  ```

Acceptance:

- `make analyze-kernel` completes and produces a reviewable warning list.
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
- `make test SMP=1` still passes after any manual fixes.

Remaining work:

- Add a CI/`make precommit-all` gate to the normal validation loop.
- After later phases introduce new patterns, broaden the check list only after
  the kernel is clean with the new set.

## Phase 4: Regression and Stress Tests

### 4.1 VFAT LFN stress test

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

### 4.2 Allocator stress

Run the existing stress tests repeatedly and under SMP:

```sh
make test-all
```

Targeted validation:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 8 --jobs 4 tests/shell/vfat.stress.test tests/shell/ext2.stress.test
```

Acceptance:

- No page allocator assertions fire.
- No timeout regressions are attributable to the debug instrumentation.

## Phase 5: Documentation and Process

- Keep `doc/VFAT_LFN_STACK_CORRUPTION_FIX.md` as the worked example.
- Add a short “memory corruption debugging” checklist that references:
  - initial-state audit,
  - allocator invariant assertions,
  - GDB watchpoints on the overwritten saved register,
  - canary and redzone options.
- Record every new make variable, C macro, and validation command in this
  plan.

## Suggested Commit Sequence

Already landed:

- `0201c63 fat: initialize LFN state before directory lookup`
- `601a48a mm: add page allocator invariant assertions`
- `51fe07d build: add clang-tidy hook and fix findings`
- `7cd1ea0 smp: keep wakeups on the task's current CPU`
- `b33e481 ahci: recover from missed shared completion IRQs`

Remaining commits, each buildable and testable on its own:

1. `fat: make LFN fragment append self-initializing`
2. `fs: initialize stateful lookup/scan objects`
3. `mm: add kzalloc() and use it for metadata`
4. `mm: add KMALLOC_DEBUG canary and double-free checks`
5. `mm: add PAGE_ALLOC_DEBUG owner/poison checks`
6. `kernel: add STACK_PROTECTOR support`
7. `kernel: add UBSAN instrumentation and freestanding handlers`
8. `build: add memory-corruption warning flags`
9. `build: add make analyze-kernel`
10. `test: add VFAT LFN stress and SMP allocator stress`
11. `doc: record memory-corruption debugging checklist and validation gate`

## Rollout Gate

Before enabling a runtime check in non-debug builds:

1. `make kernel` is warning-clean.
2. `make precommit-all` passes.
3. `make test SMP=1` and `make test SMP=4` pass.
4. `make test-all` passes.
5. The measured overhead is documented and acceptable.
