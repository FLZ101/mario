# Repository Agent Instructions

## Kernel code review

When reviewing, modifying, or reasoning about kernel code under `src/`, read and
follow the kernel safety skill at:

- `.codex/skills/kernel-code-check/SKILL.md`

Pay particular attention to user-memory copies, blocking calls, and ring-buffer
`u=1` operations inside spinlock/irqsave regions.

Before committing changes under `src/`, run:

```sh
python3 .codex/skills/kernel-code-check/scripts/check_kernel_locks.py src
make kernel
```

Fix scanner findings and build failures before committing.

## Build and QEMU test workflow

Building `make app` or `make kernel` alone does not update the bootable ramdisks
or disk images used by QEMU tests and launch targets. Rebuild the image that
matches the target under test before running it:

- `make disk-image-vbe_1024x768`: required by GUI tests (`tests/lit_gui_test.py`)
  and usable for shell/TTY tests (`tests/lit_shell_test.py`,
  `tests/lit_tty_test.py`).
- `make disk-image-text`: used by `make net-test` (`tests/net/*.py`) and text
  launch targets (`make run`, `make quick-run`).
- `make disk-image-gtk`: used by `make run-gtk` and `make debug-gtk`.

Use `.venv/bin/python` (created via `make venv`) when invoking test runners
directly, and pass targeted test files to `tests/lit_shell_test.py`,
`tests/lit_tty_test.py`, or `tests/lit_gui_test.py` before running broader
suites.

## GUI and TTY tests

- Generally avoid hardcoded absolute screen or pane positions in GUI and TTY
  tests so minor layout or placement changes do not break unrelated assertions:
  - In `tests/gui/*.test`, do not use fixed `x,y` pointer targets or fixed-rect
    screen checks by default. Use `GUI-CLICK`, `GUI-RIGHT-CLICK`, and
    `GUI-POINT` to target controls from a unique reference crop, and use
    `GUI-CHECK-SCREEN-RECT: ...,anywhere` (or `GUI-CHECK-RECT: anywhere ...`)
    to match visual features anywhere on screen rather than full-screen
    `GUI-CHECK-SCREEN` goldens or fixed coordinates. Reserve fixed coordinates
    (`GUI-MOUSE-POSITION`, `GUI-MOUSE-MOVE`, fixed `GUI-CHECK-RECT` /
    `GUI-CHECK-SCREEN-RECT`) strictly for tests whose explicit subject is
    window placement, geometry/fullscreen bounds, occlusion, or cursor motion,
    and pair visual checks with guest serial/process/filesystem assertions.
  - In `tests/tty/*.test`, prefer position-independent `TTY-CHECK`,
    `TTY-CHECK-NOT`, `TTY-CHECK-REGEX`, and `TTY-CHECK-NOT-REGEX` checks over
    full-grid `TTY-CHECK-PANEL` snapshots or row/column-locked patterns unless
    exact pane layout or border alignment is itself under test.
- Isolate writable test fixtures under unique paths in `/tmp` rather than
  mutating shared directories such as `/mnt`.

## Commits

Never skip git hooks when committing. Do not use `--no-verify`, `--no-gpg-sign`,
or any other option that bypasses `pre-commit` or `commit-msg` hooks. If a hook
fails, fix the reported issue and commit again without disabling the hook.
