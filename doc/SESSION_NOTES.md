# Mario Debugging and Test Session Notes

Date: 2026-10-16
Updated: 2026-10-17

This document summarizes the work done in this coding session.

## 1. Build system

Changed Makefile recipe shell from `/bin/sh` to `/bin/bash`, both at the top
level and in sub-makes, because some recipes use bash-only features such as
brace expansion.

Files changed:

- `Makefile`
- `src/Makefile`
- `rd/Makefile`
- `qemu/Makefile`
- `libc/Makefile`
- `musl/Makefile`
- `app/Makefile`
- `app/Makefile.in`
- `app/usr/Makefile`

## 2. Intermittent command latency

Symptom:

- Repeated `ls` and toybox commands were sometimes much slower.

Root causes found:

- Bottom halves were woken as a normal schedulable thread on every PIT tick,
  stealing CPU from the foreground command.
- MarioFS block lookup walked the linked block chain from block 1 for each
  lookup, making large executable page-in expensive.
- `mario_readdir()` read the next block pointer after `brelse()`.

Fixes:

- Run bottom halves inline from `ret_common`.
- Remove the separate bottom-half thread.
- Add MarioFS block lookup cache with contiguous-prefix detection.
- Invalidate the cache on writes/truncates/directory mutations.
- Save the next block pointer before releasing the buffer in `mario_readdir`.

## 3. File operation bugs

Fixed:

- `SYS_unlinkat` and `SYS_renameat` were mapped to `sys_not_exist`.
- `utimensat(fd, NULL, ...)` now resolves the inode from `fd`.
- `open()` no longer changes `mtime`/`ctime` on every open.
- `mario_add_entry()` invalidates the cached inode for reused directory slots.
- `mario_write_inode()` refuses to write stale inode data over a reused slot.

Verified:

```sh
touch /root/file.txt
echo hello > /root/file.txt
mv /root/file.txt /root/file2.txt
cat /root/file2.txt
rm /root/file2.txt
```

## 4. Vim save bug

Symptom:

```sh
vim main.py
:wq
```

The file was removed instead of saved.

Root cause:

- `vim_rename()` removes the destination before calling `rename()`.
- Missing `unlinkat`/`renameat` support made that path fail.
- Reused directory inode numbers could be overwritten by stale inode objects.

Fix:

- Implemented `unlinkat` and `renameat`.
- Added inode invalidation on directory slot reuse.
- Added stale-name guard in `mario_write_inode()`.

## 5. Python script bug

Symptom:

```sh
python /root/foo.py
python: can't open file '/root/foo.py': [Errno 22] Invalid argument
```

Root cause:

- Python calls `ioctl(fd, FIOCLEX, NULL)` when opening files.
- Mario did not implement `FIOCLEX`/`FIONCLEX`, returning `EINVAL`.

Fix:

- Added `FIOCLEX` and `FIONCLEX` to `src/include/fs/ioctl.h`.
- Handled them in `src/fs/ioctl.c`.

## 6. sigaltstack

Added a stub for `SYS_sigaltstack`.

The kernel does not use an alternate signal stack, so a full implementation is
unnecessary for current Mario behavior.

The stub:

- returns `0`
- zeroes `old` when provided

Files:

- `src/kernel/signal.c`
- `src/kernel/entry.S`

## 7. Test infrastructure

### Python environment

- `requirements.txt` declares `pexpect`, `Pillow`, and `pre-commit`.
- `make venv` creates `.venv` and uses `.venv/.stamp` as the rerun tag.
- `make test` runs the shell lit runner with `CASES` (default `tests/shell`).
- `make precommit` installs the pre-commit hooks.
- `.pre-commit-config.yaml` runs `black`.

### Shell behavior tests

- Runner: `tests/lit_shell_test.py`
- Cases: `tests/shell/*.test`

Directive lines no longer need a leading `#`:

```text
RUN: echo hello > /root/lit.txt
CHECK: hello
CHECK-NOT: goodbye
CHECK-REGEX: hello\s+world
```

Supported directives:

```text
RUN:
SEND:
SEND-RAW:
EXPECT:
SLEEP:
TIMEOUT:
PUSH-PROMPT:
POP-PROMPT:
EXIT:
CHECK:
CHECK-NOT:
CHECK-REGEX:
```

The parser still tolerates an optional leading `#` for backwards compatibility.

Run:

```sh
make test
make test CASES=tests/shell/fs.test
```

Current shell cases:

- `tests/shell/fs.test` - passes
- `tests/shell/oksh.test` - passes
- `tests/shell/python-0.test` - passes
- `tests/shell/python-1.test` - still SIGSEGVs in `subprocess.run(['pwd'])`

### VGA behavior tests

- Runner: `tests/lit_vga_test.py`
- Cases: `tests/vga/*.test`

Directive lines also have no leading `#`:

```text
SERIAL: /bin/example/getty.exe /dev/tty1 &
VGA-CHECK: / #
VGA-SEND: vim
VGA-CHECK: VIM - Vi IMproved
VGA-CHECK-SCREEN: vim-1.ppm,1,12
```

Supported directives:

```text
SERIAL:
VGA-SEND:
VGA-SEND-RAW:
VGA-CHECK: text
VGA-CHECK: row,col,text
VGA-CHECK-CELL: row,col,text
VGA-CHECK-NOT: text
VGA-CHECK-REGEX: pattern
VGA-CHECK-NOT-REGEX: pattern
VGA-DUMP-SCREEN: path
VGA-DUMP-SCREEN: path,start_row,end_row
VGA-CHECK-SCREEN: path
VGA-CHECK-SCREEN: path,start_row,end_row
SLEEP:
TIMEOUT:
EXIT:
```

Screen paths in `.test` files are relative to the test file unless absolute.
`VGA-SEND` translates text and common punctuation into QEMU `sendkey` events.
`VGA-DUMP-SCREEN` and `VGA-CHECK-SCREEN` can crop by terminal row before
writing/comparing.

Current VGA cases:

- `tests/vga/vim-0.test` - text checks for the Vim welcome screen
- `tests/vga/vim-1.test` + `tests/vga/vim-1.ppm` - compares welcome banner
  rows 1-12 against the golden image

Note: full-screen welcome comparison is avoided because Vim rotates the
middle charity/sponsor message based on wall-clock time.

Run:

```sh
.venv/bin/python tests/lit_vga_test.py tests/vga
```

The older `tests/behavior/*` scripts remain as prototypes; the canonical
runners are the two files under `tests/`.

## 8. Escape sequences known to be supported

From `src/fs/char/console.c`:

- ESC `c`
- ESC `=`
- ESC `>`
- ESC `7`
- ESC `8`
- ESC `D`
- ESC `M`
- ESC `( 0/A/B`
- ESC `) 0/A/B`
- CSI `H` / `f`
- CSI `A` / `B` / `C` / `D`
- CSI `G`
- CSI `d` / `e`
- CSI `s` / `u`
- CSI `m`
- CSI `J`
- CSI `K`
- CSI `L`
- CSI `M`
- CSI `P`
- CSI `X`
- CSI `@`
- CSI `n`
- CSI `r`
- CSI `l` / `h`
- CSI `b`
- CSI `S`
- CSI `t` (accepted, no-op)
- CSI `? 1 l/h`
- CSI `? 7 l/h`
- CSI `? 25 l/h`
- CSI `? 1049 l/h`
- CSI `! p`

## 9. Current status

Core kernel fixes are built into `qemu/tmp/kernel.exe`.

Shell tests:

```text
PASS: tests/shell/fs.test
PASS: tests/shell/oksh.test
PASS: tests/shell/python-0.test
```

`tests/shell/python-1.test` is still blocked on the Python `subprocess`
SIGSEGV, which is a kernel bug, not a test-harness issue.

VGA tests:

```text
PASS: tests/vga/vim-0.test
PASS: tests/vga/vim-1.test
```

`black` passes for both lit runners.
