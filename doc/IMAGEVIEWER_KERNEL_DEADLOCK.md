# imageviewer kernel deadlock (WIP)

## Summary

`tests/gui/imageviewer-png.test`, `tests/gui/imageviewer-bmp.test`, and
`tests/gui/gui-mp.test` time out in CI while waiting for imageviewer to print
`imageviewer: window ready`. The timeout is not a test bug and is not a slow
imageviewer: the optimized imageviewer triggers a kernel deadlock. The window
manager's `poll()` spins forever in `socket_poll` on a `unix_socket_pair`
whose memory has been poisoned with `0xcc`.

Two things are known-fixed, and one root-cause remains open:

1. Fixed: kmalloc returned 2-byte/1-byte aligned blocks from three buckets,
   which misaligned the 12-byte socket pair. See commit `fda830e`.
2. Fixed (workaround): imageviewer is built `-O0` regardless of the global
   `OPTIMIZE`. See commit `98d0a7e`.
3. Open: the actual write that poisons the pair while the window manager's
   socket still references it (a use-after-free / wild write) has not been
   identified.

## Symptom

CI fails like this:

```
FAIL: tests/gui/gui-mp.test [67.02s]
SERIAL 'while ! grep -q 'imageviewer: window ready' /tmp/mp-imageviewer.log ...' timed out
```

The `kill -0` bound added to the wait loops does not stop the loop, which
proves imageviewer is alive and stuck, not exiting with `window setup failed`.

Locally the same symptom appears only when imageviewer is compiled with any
optimization. `-O1`, `-O2`, `-O3`, and `-Og` all reproduce it; `-O0` works.
CI's failing jobs are `OPTIMIZE=-O3`, which is why this looked intermittent.

## Reproduction

### Trigger with an optimized imageviewer

```sh
# Build imageviewer unoptimized (baseline that works)
make -C app/usr \
  CC=/data/work/mario/util/mario-musl-cc \
  LD=/data/work/mario/util/mario-musl-ld \
  build-imageviewer

# Build imageviewer with optimization (reproduces the deadlock)
make -C app/usr clean-imageviewer
make -C app/usr \
  CC=/data/work/mario/util/mario-musl-cc \
  LD=/data/work/mario/util/mario-musl-ld \
  OPTIMIZE=-O3 build-imageviewer

# Rebuild the VBE disk image and run the failing fixture
make disk-image-vbe_1024x768
.venv/bin/python tests/lit_gui_test.py --smp 1 --jobs 1 tests/gui/imageviewer-png.test
```

The test hangs in the imageviewer readiness loop and reports the serial
timeout above. Repeat the test to observe the intermittent nature.

### Catch the frozen guest with gdb

Run the fixture against a QEMU started with a gdb stub (`-s`), wait a few
seconds after launching imageviewer, then:

```sh
gdb -batch \
  -ex 'set pagination off' \
  -ex 'target remote :1234' \
  -ex bt \
  src/kernel/kernel.exe
```

The relevant frames are:

```
#0 spin_lock (lock=0x8008a55c) at include/lib/spinlock.h:25
#1 socket_poll (...) at fs/socket.c:360
#2 poll_fds (...) at fs/poll.c:102
#3 do_poll (...) at fs/poll.c:131
#4 sys_poll (...) at fs/poll.c:234
#5 system_call ()
```

## Root-cause chain

1. The window manager is the task stuck in `socket_poll`, not imageviewer.
   imageviewer is `state=0` (`TASK_RUNNING`, runnable on the runqueue), so it
   is not holding the lock.
2. `socket_poll` spins on `socket->pair->lock`. The pair pointer is valid (it
   points at the block kmalloc returned), but the pair's memory has been
   poisoned, so the lock word never reads `0`.
3. Therefore the pair was freed/poisoned while the window manager's accepted
   socket still referenced it: a use-after-free.

### Captured corruption snapshot

At the moment of the deadlock, the pair's kmalloc block header and body read:

```
block+0  (magic)   = 0x10        # should be KMALLOC_MAGIC_ALLOC 0x6d6b616c
block+4  (owner)   = 0x0         # should be the allocating pid
block+8  (size)    = 0x7265647a  # should be 12; this is the kmalloc redzone "redz"
block+12 (board)   = 0x10        # should be a board pointer
block+16 (ep[0])   = 0x8ffc3a20  # correct first endpoint socket
block+20 (ep[1])   = 0xcccccccc  # should be the second endpoint socket
block+24 (lock)    = 0xcccccccc  # should be 0
block+28 (redzone) = 0x8008a5f8  # should be 0x7265647a
```

`0xcccccccc` is the kernel's freed-page poison byte (`PAGE_POISON_BYTE` /
kmalloc free fill), and `0x7265647a` is `KMALLOC_REDZONE_VALUE`. The block
header was therefore overwritten after allocation.

## kmalloc alignment bug (fixed)

`src/mm/kmalloc.c` had three buckets whose block sizes are not multiples of
four:

```
{NULL, 0,  34, 120}
{NULL, 0, 255,  16}
{NULL, 0, 510,   8}
```

A 12-byte `struct unix_socket_pair` lands in the 34-byte bucket, and odd block
indexes return 2-byte aligned memory. That misaligns the pair's embedded
`spinlock_t` and endpoint pointers. Fixed by rounding up and recomputing
blocks-per-board:

```
{NULL, 0,  36, 113}
{NULL, 0, 256,  15}
{NULL, 0, 512,   7}
```

This is commit `fda830e`. It is a correct allocator fix, but it only reduces
the failure; it does not eliminate it.

## Remaining use-after-free (open)

With alignment fixed and imageviewer at `-O3`, the deadlock still reproduces
roughly half the time. The pair is poisoned even though neither endpoint
socket has been closed, and no `kfree(pair)` is observed in the same run.

Attempts to catch the writer with a hardware watchpoint on `pair->lock` were
inconclusive: the watchpoint round-trips through gdb on every spin-lock/unlock,
which slows TCG enough that the race no longer triggers.

Candidate areas for the next pass:

- AF_UNIX socket lifetime / `socket_release` / `socket_destroy` refcounting.
- The kmalloc/page-allocator buddy path returning or freeing the same page
  twice.
- The ext2 / filemap block-read path interacting with freed memory.

## Current mitigations

- kmalloc is now 4-byte aligned: `fda830e`.
- imageviewer is built `-O0` regardless of global `OPTIMIZE`: `98d0a7e`.
- imageviewer readiness waits are bounded with `kill -0` and print the app log
  on failure, so future failures are self-diagnosing.

With the alignment fix and the unoptimized imageviewer, all three
imageviewer-using fixtures pass under SMP 1 and SMP 16.
