# C++ support for Mario

## Scope

Build C++17 programs on the host or in the existing build container and run
them as static 32-bit x86 executables in Mario. Cross-build LLVM's `libc++`
and `libc++abi` against Mario's musl, then add LLVM `libunwind` for exceptions.
Keep the kernel and existing C app build unchanged except where a shared
userspace ABI fix is necessary. A compiler inside Mario, shared C++ libraries,
dynamic loading, and binary compatibility with host `libstdc++` are out of scope.

Run all C++ builds under `app/usr/cxx/` on both the host and in containers,
including the runtime libraries, test probes, and apps. Keep their recipes,
build directories, and staging outputs there. The shared toolchain wrappers,
test definitions, and image assembly stay in their existing locations.

## Baseline Before M1

A host probe compiled a 32-bit C++ object and linked an `ELF32` executable
with `util/mario-musl-ld`, but its global constructor did not run. This is not
a guest result. `musl/musl.ld` does not explicitly retain the init/fini arrays
and define their boundaries for musl, or describe static TLS with `PT_TLS`.
Mario's loader in `src/fs/exec.c` requires `PT_PHDR` and passes program-header
information to userspace. The new linker layout preserves that contract.

## Milestones

### M1 — Static ELF Startup and C++ Driver

- Add `util/mario-musl-c++` for compiling and linking `.cpp` files with the
  Mario target flags, musl headers, startup objects, and linker script. Start
  with no standard library, exceptions, or RTTI. Keep compile-only and final
  link invocations separate so the driver also works with app build systems.
- Retain `.init_array` and `.fini_array` entries under `--gc-sections`, define
  the boundary symbols musl expects, and preserve constructor priorities and
  reverse finalization order. Add aligned `.tdata`/`.tbss` and `PT_TLS` to the
  static layout; verify that musl initializes TLS from the guest's program
  headers for both the initial thread and new pthreads.
- **Exit:** A guest probe verifies startup order before `main`, finalization
  on normal exit, and independent initialized and zero-initialized TLS in
  two threads. Existing C startup and pthread tests still pass.

Verified in QEMU at `SMP=1` and `SMP=2` with `cxx-startup.test`, `oksh-0.test`,
and `media-runtime.test`; the existing C `pthread.test` passed at `SMP=8`.

### M2 — Static Standard Library and ABI

- Follow the [LLVM runtime build guide](https://libcxx.llvm.org/VendorDocumentation.html)
  and existing `app/usr` recipe conventions. Use GCC 13 or later, CMake 3.20
  or later, and Ninja from the project venv. Pin LLVM 18.1.8 and fetch only
  the checksummed runtime component archives and required CMake helpers, not
  the full LLVM tree. Stage target headers and archives under `app/usr/cxx/`;
  do not install them on the host system or guest image.
- Configure CMake to cross-build static `libc++` and `libc++abi` against
  Mario's musl and compatible compiler builtins. Do not execute target
  binaries during host configuration or allow host libc or C++ dependencies
  to leak into target builds. Add the staged headers and archives to the driver.
- **Exit:** With exceptions still disabled, a guest program exercises
  `std::string`, `std::vector`, allocation and deletion, iostream output,
  function-local statics, and `std::thread` with mutex and condition-variable
  synchronization. Test a timed wait too: a successful link does not establish
  that musl's time and futex paths work in Mario.

Verified with GCC 15.2 and at `SMP=1` and `SMP=2` with `cxx-stdlib.test` and
`cxx-startup.test`. `readelf` confirms a static i386 executable with no
interpreter or dynamic dependencies.

### M3 — Exceptions, RTTI, and Thread-Local Destruction

- Build static `libunwind` from the same LLVM release. Enable unwind tables
  for C++ without changing the existing C defaults. Preserve and validate the
  unwind and exception metadata needed to discover and unwind guest frames.
- Enable exceptions and RTTI in the normal C++ build. Verify the ABI hooks for
  global destructors, thread-safe function-local static initialization, and
  `thread_local` destruction at thread exit.
- **Exit:** Guest tests catch a typed exception thrown in a separate translation
  unit, check cleanup order during unwinding and after rethrow, and exercise
  `dynamic_cast` and `typeid`. Concurrent first use initializes a local static
  exactly once; `thread_local` instances stay isolated and run their destructors
  when the worker threads exit.

Verified in QEMU at `SMP=1` and `SMP=2` with `cxx-exceptions.test`, including
the `noexcept` terminate path, alongside the startup, standard-library,
shell, and media-runtime tests. `readelf` confirms the static i386 exception
probe has TLS and mapped `.gcc_except_table`, `.eh_frame`, and `PT_GNU_EH_FRAME`.

### M4 — Normal App and Image Integration

- Have `app/Makefile.in` dispatch `.cpp` builds to `app/usr/cxx/` with C++-specific
  flags. Link C++ and mixed C/C++ apps through the C++ driver without changing
  how C-only targets link. Build the runtime as a dependency of C++ apps, not of
  kernel-only builds.
- Document how to build a standalone program and add a C++ app. Add a small
  C++17 demo under `app/usr/cxx/` with no dependencies beyond the standard
  library. Have it exercise `std::string`, `std::vector`, `std::sort`, and
  `std::accumulate` on fixed inputs and check the results. Print a success
  line only when every check passes; otherwise report the failed check and
  exit nonzero.
- **Exit:** Clean host and container builds produce the demo. Package it with
  the normal image rules and verify its success line and zero exit status in
  QEMU. The demo needs no guest data files and does not replace the focused
  runtime tests from earlier milestones.

## Validation Rules

- Inspect milestone executables with `readelf`: require `ELF32`, `EM_386`,
  `ET_EXEC`, `PT_PHDR`, and no `PT_INTERP` or `DT_NEEDED`. Check that TLS
  and unwind metadata are present and mapped where the corresponding tests
  need them, and audit the link inputs for host paths.
- Use the existing QEMU shell test runner at `SMP=1` and `SMP=2`. Require
  bounded completion, expected output and exit status, and useful failure
  logs. A host link or a boot alone does not pass a milestone.
- Keep focused probes in the normal test suite as their milestones land.
  Run existing C app and pthread regressions after shared linker or runtime
  changes. Track any unverified standard-library areas separately.
