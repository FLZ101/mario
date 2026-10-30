# PTY and tmux Enablement Plan

## Objective

Add a production-quality, Unix98-style pseudo-terminal subsystem that works
without the GUI. Use it first to run an interactive `/bin/oksh` from a PTY
test program, then use it to build and run upstream `tmux` on Mario.

The end state is a text-console workflow such as:

```sh
TERM=linux tmux new-session -s work /bin/oksh
```

tmux must create its server, allocate PTYs for panes, start shells with a
controlling terminal, deliver resize and job-control signals, detach, attach,
and clean up its local socket. The outer text console keeps its own terminal
type (`linux` for Mario VTs; the user's real terminal type for serial). tmux
sets `TERM=screen` only inside each pane. None of this depends on `/dev/fb0`,
a desktop, or a window manager.

## Scope and Decisions

### In Scope

- Unix98 master allocation through `/dev/ptmx`.
- Numbered slave terminals at `/dev/pts/N`.
- Termios, canonical input, echo, signal characters, foreground process
  groups, controlling terminals, window-size ioctls, hangup, and `poll`.
- The local `AF_UNIX` socket, `socketpair`, and one-FD `SCM_RIGHTS` transfer
  required by tmux's client/server protocol.
- Static cross-builds of `libevent` and tmux under `app/usr/`.
- PTY, socket, and tmux regression tests for SMP=1 and SMP=4.

### Explicitly Deferred

- `devpts` mount options, namespaces, per-user permissions, and dynamically
  allocated device nodes.
- Packet mode (`TIOCPKT`), remote mode, `TIOCSTI`, `TIOCGPTPEER`, and BSD PTY
  names such as `/dev/ptyXX`.
- Network sockets, TCP/UDP, abstract Unix sockets, credentials, and
  `SOCK_SEQPACKET`.
- A GUI terminal emulator. It consumes the same PTY ABI later but is not a
  dependency for this work.
- tmux plugins, control mode, UTF-8 grapheme perfection, and a full
  `screen-256color` capability database in the first passing port.

### Fixed First-Version Policies

- Support **32 PTY pairs**, numbered `0..31`. The limit is a single
  `NR_PTYS` definition; it is not an unbounded dynamic allocation project.
- Use static `/dev/ptmx` and `/dev/pts/0..31` nodes in the ramdisk. Create the
  `/dev/pts` directory during ramdisk construction; do not introduce a
  mountable devpts filesystem yet.
- Reserve character major **4** as `PTY_MAJOR`; use minors `0..31` for slaves
  and minor `255` as `PTMX_MINOR`. These values are free in the current
  `MAX_CHRDEV` table and avoid aliasing `/dev/ptmx` with a slave. They are an
  internal Mario device-number ABI; Unix98 compatibility is provided by the
  pathname and ioctl contract, not Linux's host device numbers.
- Extend the ramdisk device manifest/tooling to insert nested device paths.
  `rd/device_list` will contain `ptmx` and `pts/0..31`; `rd-make` must resolve
  each path component beneath `/dev`, reject `.`/`..` and missing/non-directory
  parents, and write the device entry into the resolved directory. Do not move
  device creation to init or rely on host `mknod` permissions.
- Use Linux-compatible Unix98 ioctl numbers and semantics wherever musl and
  tmux observe them. Do not create Mario-private replacements for
  `TIOCGPTN`, `TIOCSPTLCK`, `TIOCSCTTY`, `TIOCGPGRP`, `TIOCSPGRP`,
  `TIOCGWINSZ`, or `TIOCSWINSZ`.
- A newly allocated slave starts **locked**. `grantpt()` is a successful
  no-op in the single-root-user system; `unlockpt()` clears the lock through
  `TIOCSPTLCK`. Opening a locked, unallocated, or retired static slave returns
  `-EIO`, matching the observable Linux `/dev/pts/N` behavior rather than
  treating the persistent node as a permission error.
- Run panes with `TERM=screen` initially and package the matching terminfo
  entry. The tmux client retains the outer console's `TERM` value. The existing
  ncurses install already produces
  `share/terminfo/s/screen` and `share/terminfo/s/screen-256color`; no new
  terminfo compiler or database build is required. Add an explicit
  `rd/copy_files.py` mapping for `s/screen` in the baseline tmux change, then
  add `s/screen-256color` only after the baseline lookup and redraw tests are
  green.
- The tmux server socket lives below `/tmp/tmux-0/`. tmux requests mode `0700`
  when it creates the directory, but this single-root-user milestone does not
  add persistent Unix ownership or permission enforcement; tests assert the
  pathname and cleanup behavior, not mode isolation. The directory and socket
  are created and removed by tmux; no launch wrapper owns their lifetime.

## Implementation Status (2026-10-30)

The initial no-GUI tmux milestone is implemented and its interactive VGA
acceptance case passes. The implementation includes:

- Unix98 PTY allocation and static `/dev/pts/N` slaves, terminal/session
  handling, hangup, poll, and one shared `struct winsize` per PTY pair.
  PTY masters forward `TIOCGWINSZ` and `TIOCSWINSZ` to that shared tty state,
  so tmux can resize a pane through its master fd and deliver `SIGWINCH` to
  the pane foreground process group.
- `AF_UNIX/SOCK_STREAM`, `socketpair`, and one-FD `SCM_RIGHTS`. Descriptor
  attachments are tracked with their stream-byte start and end positions.
  A `recvmsg` copy is capped at an attachment boundary, preserving the
  association through partial reads; plain `read`/`recv` that crosses an
  attachment releases it without disturbing later stream bytes.
- tmux-required runtime compatibility: `pipe2(O_CLOEXEC|O_NONBLOCK)`,
  `ppoll`, `prctl(PR_SET_NAME|PR_GET_NAME)`, and `flock` syscall entries.
  `ppoll` accepts the Linux i386 eight-byte kernel signal-mask ABI while
  applying Mario's 32-signal mask internally.
- Console primary-device-attributes handling for `CSI c`, returning the
  VT102-compatible `CSI ? 6 c` response tmux probes during startup.
- Static libevent 2.1.13 and tmux 3.8 builds, installed into the ramdisk with
  the ncurses `screen` terminfo entry.

The passing VGA test creates a real tmux server, sends distinct commands to
three panes, creates a second window, returns to the split window, checks
that the server remains alive, and compares the final tmux framebuffer. The
test suppresses tmux's live clock in its local configuration so the checked-in
PPM remains deterministic. The broader detach/reattach, resize-stress, and
repeated-lifecycle goals below remain the next acceptance coverage to add.

## Current Baseline

Mario already has useful pieces but not the complete contract:

- `src/fs/char/tty.c` implements tty line discipline, canonical/noncanonical
  reads, echo, signal-character handling, termios ioctls, `poll`, and checks
  against the tty foreground process group.
- Process/session fields and process-group signal helpers already exist, and
  syscall numbers for `setpgid`, `getpgrp`, and `setsid` are present.
- The anonymous pipe implementation supplies bounded ring buffers and
  readiness waiting, but a pipe is not a terminal and cannot replace a PTY.
- `src/include/fs/ioctl.h` has common tty ioctls, including controlling-tty,
  foreground-pgrp, and window-size numbers, but lacks the Unix98 PTY ioctls.
- The static ramdisk currently exposes tty and serial nodes but no `/dev/ptmx`
  or `/dev/pts` nodes.
- ncurses and oksh are already vendored and statically built. tmux additionally
  requires its event-loop dependency and a local client/server socket
  transport; PTY support by itself is not enough to run unmodified tmux.

Before code changes, record the exact output of these baseline tests:

```sh
make test SMP=1 JOBS=1
make test SMP=4 JOBS=4
```

Any failures outside PTY/sockets must be resolved or explicitly excluded before
using them as regressions for this project.

## Architecture

```text
                  client process / tmux client
                             |
                     AF_UNIX SOCK_STREAM
                             |
                    tmux server process
                      |              |
                   master 0       master N
                      |              |
                  /dev/ptmx     /dev/ptmx
                      |              |
              +-------+--------------+-------+
              |      PTY pair slot N         |
              |  master->slave input queue   |
              |  slave->master output queue  |
              |  shared termios/winsize      |
              |  session, ctty, fg pgrp      |
              +---------------+--------------+
                              |
                       /dev/pts/N slave
                              |
                 setsid + TIOCSCTTY + exec
                              |
                         /bin/oksh/job
```

### Data Direction

The data paths must remain distinct:

- A **master write** injects bytes into the slave's input side. Those bytes
  pass through the existing slave tty line discipline: canonical buffering,
  echo, `ISIG`, `ICRNL`, and other enabled termios transforms apply.
- A **slave read** consumes processed input.
- A **slave write** produces terminal output. It is queued for a master read;
  output post-processing such as `ONLCR` applies when enabled.
- A **master read** consumes slave output bytes exactly as a terminal emulator
  or tmux server would see them.

The master is not itself a terminal: `TCGETS` and controlling-terminal ioctls
on the master fail with `-ENOTTY`. The slave owns the `tty_struct`, termios,
window size, session, and foreground process group.

### PTY Slot State

Each fixed slot contains:

```c
enum pty_state {
    PTY_FREE,
    PTY_ALLOCATED_LOCKED,
    PTY_ACTIVE,
    PTY_HUNG_UP,
};

struct pty_pair {
    spinlock_t lock;
    enum pty_state state;
    unsigned int number;
    unsigned int master_opens;
    unsigned int slave_opens;
    int slave_locked;
    int master_hung_up;
    int slave_hung_up;
    struct tty_struct slave_tty;
    /* slave_tty.read_buf is the master-to-slave input queue. */
    struct ring_buffer master_output_buf;
    wait_queue_t master_read_wait;
    wait_queue_t master_write_wait;
    wait_queue_t slave_write_wait;
};
```

State transitions are fixed:

| Event | Result |
| --- | --- |
| `open("/dev/ptmx")` on `PTY_FREE` | Reserve the lowest slot as `PTY_ALLOCATED_LOCKED`; create one master open. |
| `TIOCSPTLCK(0)` / `(1)` | Toggle only admission of new slave opens. Existing slave fds remain valid. An unlocked non-hung-up slot is `PTY_ACTIVE`; relocking does not destroy it. |
| Final master release with no slave opens | Reset and publish `PTY_FREE` immediately. |
| Final master release with slave opens | Publish `PTY_HUNG_UP`, reject new slave opens with `-EIO`, hang up existing slaves, and retain the slot until their final releases. |
| Final slave release while a master remains | Keep the allocated master slot active; master reads drain queued output then return `-EIO`, and master writes return `-EIO`, until a later slave open. Do not reset termios, winsize, session, pgrp, or queued master input: the controlling session may still own the tty without an open slave fd. |
| Final slave release after master hangup | Reset and publish `PTY_FREE`. |

The exact layout may differ, but these ownership properties are mandatory:

- `master_opens` and `slave_opens` count successful `open(2)` operations--that
  is, distinct open file descriptions. `fork`, `dup`, and descriptor
  inheritance increment `struct file::f_count` only, so they do **not** change
  either counter. The endpoint count is decremented exactly once from the
  file's final `.release` callback.
- The slot lock protects state, endpoint-open counts, lock state, hangup
  state, and the master-output queue. The tty core owns the master-to-slave
  input queue (`slave_tty.read_buf`). The embedded tty lock continues to
  protect termios, line-discipline state, window size, session, and foreground
  pgrp.
- Lock ordering is always **PTY slot lock -> tty lock -> tasklist lock**.
  Never acquire the slot lock while holding either later lock. Snapshot every
  pgrp/session value needed for signaling while locked, then drop all locks
  before calling `kill_pg()`, waking a wait queue, copying to/from userspace,
  sleeping, or executing filesystem callbacks.
- A slot returns to `PTY_FREE` only after both endpoint-open counts reach
  zero. The final release discards both queues, resets the embedded tty, wakes
  all waiters, and then publishes `PTY_FREE`; no reader may observe a reused
  slot through an old `struct file`.

## Kernel ABI

### Device Nodes and Numbers

Register `PTY_MAJOR` (major `4`) separately from the existing tty major and
document it with `NR_PTYS` and `PTMX_MINOR` in `src/include/fs/chrdev.h`.
Extend `rd/device_list` and `rd/tools/rd-make.c` so the manifest can resolve a
nested pathname beneath `/dev`, then add these ramdisk nodes:

```text
ptmx       C 4 255
pts/0      C 4 0
...
pts/31     C 4 31
```

The minor-to-slave mapping is stable: slave minor `N` names PTY `N`; minor 255
is exclusively `/dev/ptmx`. The manifest implementation must create `/dev/pts`
as a directory in the image and reject a component that traverses outside
`/dev`; it must not encode `pts/0` as a flat filename.

### Required ioctls

Add Linux-compatible definitions and handlers for:

| Operation | Master | Slave | Required behavior |
| --- | --- | --- | --- |
| `TIOCGPTN` (`0x80045430`) | yes | no | Copy the allocated slave number to user memory. |
| `TIOCSPTLCK` (`0x40045431`) | yes | no | Set/clear the slave-open lock from an `int`. |
| `TCGETS`, `TCSETS*` | no | yes | Reuse current tty termios behavior. |
| `TIOCSCTTY` | no | yes | Make this slave the caller's controlling tty after `setsid()`. |
| `TIOCNOTTY` | no | yes | Detach the caller/session from its controlling tty. |
| `TIOCGPGRP`, `TIOCSPGRP` | no | yes | Get/set a valid foreground pgrp in the same session. |
| `TIOCGSID` | no | yes | Report the controlling session. |
| `TIOCGWINSZ`, `TIOCSWINSZ` | yes | yes | Share rows/columns/pixels with the slave and signal resize. |

Phase-1 error rules:

- Wrong endpoint or unsupported tty command: `-ENOTTY`.
- A locked, unallocated, or retired static slave returns `-EIO`; an invalid
  device minor returns `-ENXIO`.
- A malformed user pointer returns the existing user-copy fault error. Reject
  `TIOCSPTLCK` values other than `0` and `1`, and a zero pgrp, with `-EINVAL`.
  `struct winsize` accepts zero dimensions because applications use zero to
  mean unknown; no dimensions are rejected merely for being zero.
- `TIOCSCTTY` succeeds only for a session leader with no different controlling
  tty when the slave is unowned or already owned by that session; otherwise it
  returns `-EPERM`. Its argument must be zero in this milestone; the Linux
  force-steal form is deliberately unsupported and returns `-EINVAL`.
- `TIOCGPGRP`, `TIOCSPGRP`, and `TIOCGSID` require that the caller's session
  control this slave, else return `-ENOTTY`. `TIOCSPGRP` returns `-EPERM` if
  the requested nonzero pgrp does not exist in that session.

### Open, Close, EOF, and Hangup Semantics

1. Opening `/dev/ptmx` reserves the lowest free slot, initializes queues and
   tty defaults, creates a master file, marks its slave locked, and returns
   the master fd.
2. `TIOCGPTN` exposes the slot number; `TIOCSPTLCK(0)` unlocks the matching
   static slave pathname.
3. Opening `/dev/pts/N` increments `slave_opens`. A session leader with no
   controlling tty acquires an unowned slave automatically unless `O_NOCTTY`
   is set. `TIOCSCTTY` must also acquire an unowned slave for a session leader
   that inherited a pre-opened slave fd, which is the tmux child sequence:
   `setsid(); ioctl(slave_fd, TIOCSCTTY, 0); dup2(); exec()`.
4. On final master close, mark master hangup, wake slave readers/writers and
   pollers, deliver `SIGHUP` then `SIGCONT` to the slave foreground pgrp, and
   discard all unread master-to-slave input before making slave reads return
   EOF. This applies in both canonical and noncanonical mode: an unterminated
   canonical line must not leave a reader blocked after hangup. Slave writes
   return `-EIO`.
5. On final slave close, wake master waiters. Once queued output drains,
   master reads and master writes return `-EIO`.
6. A pty does not become reusable until both endpoint-open counts are
   zero. Reusing a slave number must reset termios, queues, winsize, session,
   foreground pgrp, and hangup flags.

A later slave open while the master remains open reattaches to that same live
terminal state; it is not a new PTY allocation. Only final master close (with
no slaves) or final slave close after master hangup resets the state. This
prevents a different session from acquiring a still-owned tty merely because
its original slave fd was closed.

`TIOCNOTTY` from the session leader detaches the whole owning session: it
clears `current->tty` for every task in that session that references this tty,
clears the tty session/pgrp association, and sends `SIGHUP` then `SIGCONT` to
the previous foreground pgrp after locks are dropped. For a non-leader, it
only clears that task's `current->tty` pointer and does not detach or signal
the session. Session-leader exit performs the same one-time session detach. A
close of an ordinary slave fd does not detach the session merely because it was
the final slave open.

### Read, Write, poll, and Resize

- Use 4096-byte bounded queues in each direction initially. The slave tty's
  `read_buf` is the master-to-slave input queue; `master_output_buf` is the
  slave-to-master output queue. Blocking writes sleep interruptibly only when
  their destination queue is full. A write that copied any bytes returns that
  short positive count; a nonblocking write that copied none returns
  `-EAGAIN`.
- Master `POLLIN` means slave output is queued; master `POLLOUT` means a slave
  is open and its input queue has room; master `POLLHUP|POLLERR` means all
  slave opens are gone. Slave `POLLIN` means processed input or EOF after a
  master hangup; slave `POLLOUT` means a master is open and the output queue
  has room; slave `POLLHUP|POLLERR` means master hangup. Register poll waiters
  before the final readiness check, then wake after dropping the queue lock.
- PTY master read/write must not run terminal job-control checks. Slave
  read/write use the shared tty checks, so `SIGTTIN`/`SIGTTOU` apply only to
  processes in the slave's controlling session.
- `TCSETS` applies immediately. On a PTY slave, `TCSETSW` waits until
  `master_output_buf` drains, and `TCSETSF` does the same then discards pending
  master-to-slave input before applying the attributes. Both waits are
  interruptible and wake when a master read drains output. Existing console
  and serial behavior stays unchanged during the tty-core refactor unless its
  current tests require the same correction.
- `TIOCSWINSZ` updates a single `struct winsize` stored with the slave tty.
  If rows or columns changed, signal `SIGWINCH` to the foreground pgrp after
  releasing locks. Pixel fields are retained even though the text console has
  no pixel geometry.
- PTY slave input invokes the reusable tty receive path; it must not duplicate
  console keyboard code. This is the key refactor that lets keyboard, serial,
  and master input share canonical-mode and signal handling.

## Required Supporting Kernel Work

### 1. Refactor the tty Core Without Behavior Changes

Split `src/fs/char/tty.c` into a reusable tty core and endpoint-specific
adapters before adding PTYs:

- `tty_core_init()`, termios defaults, canonical processing, echo/output
  processing, `tty_receive_byte()`, `tty_read()`, `tty_write()`, `tty_poll()`,
  hangup, and winsize helpers are endpoint-neutral.
- Console and serial adapters provide hardware input/output hooks.
- The PTY slave adapter sends output to the master-output queue and accepts
  input from the master-input queue.
- Replace the current `void put_char()` driver callback with an output-sink
  contract that can report room, short progress, and an interruptible wait.
  The tty core performs `OPOST`/`ONLCR` exactly once; the PTY sink enqueues the
  transformed bytes in `master_output_buf`, while console and serial sinks
  retain their immediate hardware output behavior.
- Split input delivery into a non-sleeping hardware path and a blocking
  process-context path. Keyboard/serial input may return a documented
  backpressure/drop result and must never sleep in IRQ context. A PTY master
  write waits for room in both the slave input queue and any echo output it
  will generate, then injects one complete input character without dropping
  it. Add a slave-input-space wait queue and wake it whenever slave reads or
  input flushes free space. Never hold the tty or PTY slot lock while waiting.
- Echo is part of the same flow-control contract: a saturated master-output
  queue backpressures a master write instead of silently losing echo. Preflight
  the bounded expansion of one input byte (including `ONLCR`, `ECHOCTL`, and
  `ECHOE`) before mutating the canonical input queue, so an interrupted write
  returns only bytes whose input and echo state were both committed.
- Refactor `/dev/tty` resolution before adding PTYs. When the `TTY_MINOR`
  alias is opened, bind the new file directly to `current->tty` through a
  core `tty_bind_file()`/endpoint callback; do not resolve it again through
  `get_tty(MINOR(current->tty->dev))`. The callback selects the correct slave
  file operations and increments its endpoint-open count. This preserves
  existing VT/serial behavior and makes `/dev/tty` work when the controlling
  tty's device major is `PTY_MAJOR`.
- Existing `/dev/tty0..6`, `/dev/console`, serial, `poll_tty`, and job-control
  behavior must remain byte-for-byte compatible before proceeding. Add a
  regression that opens `/dev/tty` after a PTY `TIOCSCTTY` and verifies it is
  the same slave as the inherited fd.

### 2. Make `poll` Registration Race-Free

PTYs and Unix sockets add multiple independently wakeable queues per fd. The
current poll collector records queues first and attaches one wait node only
after its first readiness scan, which can miss a wakeup; one wait node also
cannot safely sit on more than one queue. Fix this shared framework before
either subsystem relies on it:

- Give every `(file, wait_queue)` registration its own `wait_queue_node_t` and
  retain a file reference for the whole `poll` call, including the sleep and
  final rescan. Allocate the registration array from `nfds` rather than relying
  on `MAX_POLL_ENTRIES` for a tmux-sized fd set.
- Collect poll queues, mark the caller interruptible, attach all registration
  nodes, and rescan readiness with registration disabled before calling
  `schedule()`. Remove every node and release every retained file reference on
  ready, signal, timeout, allocation failure, and copy-out failure paths.
- Keep the current `poll_wait()` interface for tty, pipe, PTY, and socket file
  operations. It records a queue in the collection pass and performs no
  allocation or user-memory access while the endpoint lock is held.
- Add an SMP regression that deliberately wakes a PTY/socket readiness queue
  between collection and sleep; it must never consume the full timeout.

### 3. Complete Session and Controlling-TTY Rules

Audit and complete `setsid`, `setpgid`, `exec`, `fork`, and exit paths:

- Keep `task_struct::tty` as the per-task fast path, but make
  `tty_struct::session` the authoritative session owner. A child inherits the
  pointer across fork and retains it across exec only while its session still
  owns that tty. `setsid()` must first detach the caller from its inherited
  controlling tty before assigning the new session/pgrp.
- On session-leader exit or `TIOCNOTTY`, detach every task in the owning
  session from that tty under the tasklist lock, clear the tty association, and
  perform `SIGHUP`/`SIGCONT` exactly once after locks are dropped.
- In `do_exit()`, perform that session-leader detach **before** `close_files()`
  releases the exiting task's slave descriptors. Snapshot the foreground pgrp,
  clear all session task pointers, and clear `tty->session`/`tty->pgrp` first;
  then deliver `SIGHUP` and `SIGCONT`, and only then allow final endpoint
  release to reset or reuse a PTY slot. A non-leader exit only closes its own
  descriptors and never detaches the shared session.
- Reject `TIOCSPGRP` unless the target pgrp exists and is in the controlling
  tty's session. Background read/write checks use that same shared state.
- Preserve current `SIGINT`, `SIGQUIT`, `SIGTSTP`, `SIGTTIN`, and `SIGTTOU`
  behavior; add `SIGWINCH`, `SIGHUP`, and `SIGCONT` only where the PTY
  lifecycle requires them.

### 4. Add the PTY Character Driver

Suggested files:

```text
src/include/fs/pty.h
src/fs/char/pty.c
src/fs/char/tty_core.c        # only if the refactor warrants a new file
src/include/fs/ioctl.h
src/include/fs/tty/tty.h
src/fs/char/tty.c
rd/device_list
rd/make-rd0.py                # if nested device directories need support
```

The driver registers during `tty_init()` after the reusable tty core exists.
It must be usable before init launches user programs.

The PTY driver owns the device nodes and endpoint files; it does not register
its slaves with the existing `tty_driver` minor table. Its slave endpoint
implements the core callback used by `/dev/tty`. Add an explicit endpoint tag
in `file::private_data` so a master and slave can never be confused after
allocation/reuse. `open("/dev/pts/N")` resolves the fixed minor to a live slot
only while that slot is allocated; it must never create a PTY.

## Minimal AF_UNIX Sockets for tmux

tmux is a client/server application. A PTY implementation allows panes but
does not replace the local control socket used by `tmux`, `tmux attach`,
`tmux detach`, `tmux list-sessions`, and `tmux kill-server`.

Deliver a deliberately narrow socket subsystem. This is intentionally larger
than a pathname stream listener: tmux must create an internal pair and pass
the client's controlling-terminal fd to its server.

### VFS and File-Object Representation

Keep every socket file inode-backed, matching Mario's existing anonymous-pipe
model, rather than weakening the generic `read`, `write`, final-release, and
`fstat` paths that require `file::f_inode`:

- Add `get_socket_inode()` beside `get_pipe_inode()`. An anonymous socketpair
  or accepted stream connection allocates an `S_IFSOCK` inode, attaches the
  shared Unix-socket endpoint object through `file::private_data`, and gives
  each returned file a normal inode reference. Its socket file operations
  provide `read`, `write`, `lseek -> -ESPIPE`, `poll`, and final `.release`.
- Extend the inode representation with a filesystem-independent private
  socket-binding pointer and destructor hook. A pathname `bind` creates an
  `S_IFSOCK` inode through the mounted filesystem and holds one binding
  reference in that inode; `connect` resolves the inode and obtains a separate
  endpoint reference. `unlink` removes the name and drops only the inode's
  binding reference, not active listener or connection endpoints.
- Teach MarioFS and ext2 inode loading/creation to preserve `S_IFSOCK` and
  select the Unix-socket file operations. FAT does not gain pathname Unix
  sockets in this milestone; reject `bind` when its parent is FAT with
  `-EOPNOTSUPP`. Socketpair remains available on every filesystem because its
  endpoints are anonymous.
- `fstat` on a socketpair, connected endpoint, listener, or bound pathname
  reports `S_IFSOCK`. All normal `iput` and final-file-release paths must drop
  their socket references exactly once; add leak counters for anonymous inodes
  and namespace bindings.

- Families/types: `AF_UNIX` + `SOCK_STREAM` only.
- **i386 syscall ABI:** Mario's bundled musl 1.2.5 routes its socket wrappers
  through `SYS_socketcall` (102), even though the newer direct socket syscall
  numbers are defined. Implement the `socketcall` dispatcher first, with the
  Linux operation numbers and userspace argument-array validation for `socket`,
  `bind`, `connect`, `listen`, `accept`, `getsockname`, `getpeername`,
  `socketpair`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`,
  `setsockopt`, `getsockopt`, `sendmsg`, `recvmsg`, and `accept4`. Direct
  syscalls 359--373 must dispatch to the same helpers before this phase exits;
  do not maintain two socket implementations.
- C operations: `socket`, `socketpair`, `bind`, `listen`, `accept`/`accept4`,
  `connect`, `send`/`recv`, `sendto`/`recvfrom`, `sendmsg`/`recvmsg`,
  `shutdown`, `getsockname`, `getpeername`, `fcntl`, `poll`, and `close`.
  Support `SOCK_NONBLOCK` and `SOCK_CLOEXEC` at `socket`, `socketpair`, and
  `accept4` creation time. Implement `getsockopt(SOL_SOCKET, SO_ERROR)` and
  return `-ENOPROTOOPT` for all unsupported socket options until the port audit
  names a tested additional option.
- Ancillary data: support exactly one `SCM_RIGHTS` fd per `sendmsg` message on
  an `AF_UNIX/SOCK_STREAM` socket. The queued message takes a file reference;
  successful `recvmsg` installs it in the receiver's fd table, while failed
  receive, peer close, and queue teardown release it. Reject all other
  `cmsg_level`, `cmsg_type`, and multi-fd payloads with `-EINVAL`. Test no fd
  leak on every failure path. An FD-bearing `sendmsg` must include at least one
  data byte. Attach that fd to the first byte of that send; the first
  `recvmsg` consuming that byte receives it. A plain `read`/`recv` consuming
  the tagged byte, a too-small control buffer (`MSG_CTRUNC`), or a full
  receiver fd table discards and releases the attachment rather than stalling
  or leaking it. Preserve the data stream in each case. Queue each attachment
  with its `[start, end)` stream-byte range and cap a descriptor-aware receive
  at the next boundary, so a partial receive cannot skip, attach early, or
  reorder a later descriptor.
- Addressing: pathname sockets below `/tmp` only. Enforce `sun_path` bounds,
  require the parent directory to exist, and create an `S_IFSOCK` filesystem
  entry atomically at bind. `close` never unlinks that entry; `unlink` removes
  the name while active connected endpoints stay valid. A stale entry after a
  server crash remains until tmux unlinks it, as on Unix. `connect` to a stale
  unlistening pathname returns `-ECONNREFUSED`.
- Backlog: a bounded pending-connect queue; `connect` blocks or returns
  `-EAGAIN` under `O_NONBLOCK` while full. A successful enqueue completes
  `connect` with zero without waiting for `accept`; listener close drains its
  pending queue and wakes blocked connectors with `-ECONNREFUSED`.
- Read/write behavior: `read`/`write` and `send`/`recv` share the stream queue
  implementation. Order byte data and its one optional fd attachment exactly
  as sent; partial stream reads must not lose or reorder an attachment.
- Each connected direction starts with a 4096-byte bounded queue. Readability
  means queued bytes or peer write-shutdown; writability means local write is
  enabled, the peer read side is open, and the peer queue has room. A peer's
  final close or `SHUT_WR` returns EOF only after queued bytes drain. Local
  `SHUT_WR` rejects later writes; peer read shutdown or final close rejects
  writes with `-EPIPE` and sends `SIGPIPE` unless the call uses
  `MSG_NOSIGNAL`. Support `SHUT_RD`, `SHUT_WR`, and `SHUT_RDWR`; reject other
  `how` values with `-EINVAL`.
- Listener `POLLIN` means at least one pending connection; connected endpoint
  `POLLIN`/`POLLOUT` follow the queue rules above, and `POLLHUP|POLLERR` is
  reported after peer close or terminal connection failure. A socket never
  reports `POLLOUT` merely because it is a listener.
- No networking, abstract names, credentials, datagrams, or `SOCK_SEQPACKET`
  in the first tmux milestone.

The socket implementation is its own mergeable milestone. Its tests must pass
before tmux is introduced, so tmux build failures do not mask kernel socket
bugs.

## Userspace Build Plan

### Dependency Order

1. Existing musl, ncurses, and oksh remain the terminal baseline.
2. Build and install `libevent` by itself under `app/usr/`.
3. Build and install tmux under `app/usr/`, with `build-tmux` depending on the
   completed libevent install and linking only against that install plus the
   existing ncurses install.
4. Copy `s/screen` from the existing ncurses install into the ramdisk through
   `rd/copy_files.py`, alongside the current `ansi`, `linux`, `vt100`,
   `vt102`, and `xterm` entries. Add `s/screen-256color` later if enabled.
5. Add the PTY/socket test helpers to the ramdisk.

Do not add a GUI dependency. tmux runs on existing `/dev/ttyN` text VTs and
serial consoles once the PTY and socket work is complete.

### `libevent`

Follow `app/usr/vim.mk` and `app/usr/build-vim.sh` directly. Add a standalone
package pair with the same source/download/install pattern:

```text
app/usr/libevent.mk
app/usr/build-libevent.sh
```

Pin libevent to release `2.1.13-stable`:

```make
LIBEVENT_VER         := 2.1.13-stable
LIBEVENT_SRC_DIR     := libevent-$(LIBEVENT_VER)
LIBEVENT_SRC_FILE    := $(LIBEVENT_SRC_DIR).tar.gz
LIBEVENT_SRC_URL     := https://github.com/libevent/libevent/releases/download/release-$(LIBEVENT_VER)/$(LIBEVENT_SRC_FILE)
LIBEVENT_INSTALL_DIR := $(ROOT)/$(LIBEVENT_SRC_DIR)-install
```

`libevent.mk` exports `LIBEVENT_INSTALL_DIR`, fetches/extracts the source, and
provides `build-libevent`, `clean-libevent`, and `clean-src-libevent` targets
in the Vim style. `build-libevent.sh` receives `CC`, `LIBEVENT_SRC_DIR`, and
`LIBEVENT_INSTALL_DIR`, exits early when the install directory already exists,
and installs only static headers and archives into that install directory.

Build libevent independently--it must not reference tmux, ncurses, the
ramdisk, or tmux's configure cache. Disable examples, tests, benchmarks,
shared libraries, thread support, and unneeded DNS/HTTP features unless the
tmux port audit names a tested requirement. Keep poll/select support enabled
because it is the required event backend on Mario. Its phase gate is a
successful standalone cross build with the expected headers and static archive
under `LIBEVENT_INSTALL_DIR`; no target binary is run on the host.

### tmux

Follow the same Vim pattern for:

```text
app/usr/tmux.mk
app/usr/build-tmux.sh
```

Pin tmux to release `3.8`:

```make
TMUX_VER         := 3.8
TMUX_SRC_DIR     := tmux-$(TMUX_VER)
TMUX_SRC_FILE    := $(TMUX_SRC_DIR).tar.gz
TMUX_SRC_URL     := https://github.com/tmux/tmux/releases/download/$(TMUX_VER)/$(TMUX_SRC_FILE)
TMUX_INSTALL_DIR := $(ROOT)/$(TMUX_SRC_DIR)-install
```

`tmux.mk` provides `build-tmux`, `clean-tmux`, and `clean-src-tmux` targets in
the same form as `vim.mk`. Declare:

```make
build-tmux: build-libevent src-tmux
```

so tmux never configures or links until the standalone libevent install is
complete. `app/usr/Makefile` includes `libevent.mk` before `tmux.mk`.

`build-tmux.sh` receives `CC`, `TMUX_SRC_DIR`, `TMUX_INSTALL_DIR`,
`LIBEVENT_INSTALL_DIR`, and `NCURSES_INSTALL_DIR`. It must:

- use `util/mario-musl-cc` and static link flags;
- fail immediately if either dependency install directory is absent;
- pass explicit `CPPFLAGS` for `${LIBEVENT_INSTALL_DIR}/include`,
  `${NCURSES_INSTALL_DIR}/include`, and `${NCURSES_INSTALL_DIR}/include/ncursesw`;
- pass explicit static `LDFLAGS` for `${LIBEVENT_INSTALL_DIR}/lib` and
  `${NCURSES_INSTALL_DIR}/lib`, without host `pkg-config` fallback paths;
- add a conservative cross-configuration cache only for a probe documented in
  `config.log` and `doc/tmux-port-audit.md`;
- disable optional features only after recording the missing Mario API and
  deciding it is outside this milestone;
- never run a target binary on the host during configure;
- install only into `TMUX_INSTALL_DIR`, never directly into the ramdisk.

`rd/copy_files.py` is solely responsible for packaging. Add the concrete
Vim-style mapping `app/usr/tmux-3.8-install/bin/tmux -> usr/bin/tmux`, plus
the existing ncurses install's `s/screen` mapping. Do not try to interpolate
Make variables inside the Python mapping. A tmux-version bump updates
`tmux.mk`, the concrete copy path, and the source/install cleanup paths
together. Because tmux links libevent statically, do not package a libevent
shared object or duplicate its headers in the ramdisk.

Before changing the kernel to suit a configure test, capture `config.log` and
classify each failure as one of: missing header/constant, missing syscall,
missing libc wrapper, missing terminal database file, or true kernel behavior
gap. Add compatibility definitions only when the underlying behavior exists
and has a direct regression test.

### Required libc and Runtime Audit

Create a checked-in `doc/tmux-port-audit.md` during implementation. It tracks
every tmux/libevent requirement with status and test coverage:

| Area | Expected requirement | Plan action |
| --- | --- | --- |
| Terminal | `openpty` or `posix_openpt`, `grantpt`, `unlockpt`, `ptsname` | Implement Unix98 PTY ABI; use musl wrappers where available. |
| Session | `fork`, `setsid`, `setpgid`, `ioctl(TIOCSCTTY)` | Validate session/ctty behavior with a native test before tmux. |
| I/O | `poll`, `ppoll`, nonblocking fds, `pipe2`, `fcntl` | Implemented the tmux-required `pipe2` and `ppoll` ABI; retain focused regression coverage for their flags, timeout, and signal-mask paths. |
| Runtime | `prctl(PR_SET_NAME/PR_GET_NAME)`, `flock` | Implemented the tmux-required syscall entries; broaden lock semantics only when a multi-server regression requires it. |
| Local IPC | pathname stream socket, `socketpair`, one-FD `SCM_RIGHTS` | Implement and test the minimal tmux transport milestone. |
| Terminal database | `TERM=screen` entry | Package tested terminfo files in the ramdisk. |
| Filesystem | `/tmp`, `mkdir`, `unlink`, permissions | Verify socket directory creation and stale-socket cleanup. |
| Signals | `SIGCHLD`, `SIGHUP`, `SIGWINCH`, process groups | Add/verify semantics before pane job-control tests. |
| Console geometry | outer `TIOCGWINSZ`, pane `TIOCSWINSZ` | Ensure a tmux pane inherits and updates text-VT dimensions. |

## Implementation Sequence

### Phase 0: Baseline and Test Scaffolding

Add a native `app/test/pty/` program and shell-test wrapper, but have it
report `SKIP` until `/dev/ptmx` exists. Add tests that verify the current text
VT and pipe behavior has not changed. Before socket or userspace package code,
run a compile-only tmux/libevent feature probe against the Mario headers and
record every referenced syscall, socket option, libc wrapper, header, and
configure result in `doc/tmux-port-audit.md`. The audit is a scope gate: a
missing primitive must be either added to a named phase with a native test or
explicitly deferred by selecting a build option that removes it; it cannot be
silently papered over in a configure cache.

Exit gate:

- Existing `poll_tty`, `select_tty`, pipe, signal, oksh, sbase, and Python
  subprocess tests pass at SMP=1 and SMP=4.

### Phase 1: tty-Core Refactor

Extract endpoint-neutral tty behavior with no PTY nodes and no ABI changes.

Exit gate:

- Existing tty tests are unchanged.
- A new in-kernel/unit-style test or temporary loopback adapter feeds bytes
  through canonical mode, echo, Ctrl-C, Ctrl-Z, and noncanonical reads.
- No locking assertion, lock inversion, or lost-wakeup regression under
  repeated SMP=4 pipe/tty stress.
- The shared `poll` registration test passes for a pipe and a temporary tty
  loopback adapter before PTY or socket queues are introduced.

### Phase 2: Basic PTY Pair

Implement the nested device-manifest support, `/dev/ptmx`, static slaves, slot
allocation, `TIOCGPTN`, lock/unlock, master/slave data flow, short
reads/writes, `poll`, and endpoint-open accounting.

Exit gate:

- Native test allocates a master, unlocks its slave, opens it, and verifies
  master-to-slave and slave-to-master byte flow.
- Locked slave open fails; unlocking allows open; the lowest released slot is
  deterministically reused.
- EOF, `EIO`, `POLLHUP`, blocking wakeups, and `O_NONBLOCK` behavior have
  explicit tests.

### Phase 3: Termios, Session, and Job Control

Connect master input through the slave line discipline; implement ctty,
foreground pgrp, winsize, `SIGWINCH`, and final-master-close hangup.

Exit gate:

- Both child sequences yield `isatty(0) == 1`: `setsid(); open(slave);
  dup2(); exec(oksh)` (automatic acquisition) and `setsid();
  ioctl(inherited_slave, TIOCSCTTY, 0); dup2(); exec(oksh)` (tmux-compatible
  inherited fd).
- Ctrl-C sent through the master interrupts `sleep`; Ctrl-Z stops a foreground
  command; `fg`/`bg` still work in oksh.
- `TIOCSWINSZ` changes `stty size` in the slave and delivers `SIGWINCH`.
- Final master close delivers `SIGHUP` then `SIGCONT` to the foreground pgrp.

### Phase 4: AF_UNIX tmux Transport Milestone

Implement and test pathname `AF_UNIX/SOCK_STREAM`, `socketpair`, and one-FD
`SCM_RIGHTS` independently of tmux.

Exit gate:

- Two native processes bind, listen, connect, `accept4`, exchange bytes via
  `poll`, handle a nonblocking full queue, then unlink the pathname explicitly.
- A `socketpair` test transfers an open fd in both directions, proves that the
  receiver can use it after the sender closes its copy, and proves every error
  path releases its queued file reference.
- The native socket test invokes both the musl `socketcall` wrappers and the
  direct syscall numbers, proving they share one state machine and return the
  same readiness, close, and error behavior.
- Close/hangup and stale-path behavior--including `-ECONNREFUSED` before
  explicit unlink--are deterministic across 100 SMP=4 repetitions.

### Phase 5: libevent and tmux Cross Builds

First run `make -C app/usr build-libevent` and validate its standalone install
tree. Then run `make -C app/usr build-tmux`; its declared dependency consumes
that libevent install and the existing ncurses install. Record configure
decisions, package tmux plus terminfo through `rd/copy_files.py`, and boot with
the binary available in `/usr/bin/tmux`.

Exit gate:

- `tmux -V` runs on Mario.
- Removing `LIBEVENT_INSTALL_DIR` makes `build-tmux` fail before configure;
  rebuilding libevent restores the tmux build without any source-tree edits.
- The ramdisk dependency printout names the concrete tmux binary and
  `s/screen` terminfo inputs; deleting either input makes the image build fail
  rather than silently omitting tmux support.
- `tmux -L smoke -f /dev/null new-session -d -s smoke /bin/oksh` creates a
  server and PTY-backed pane without a user configuration file.
- `tmux -L smoke list-sessions`, `capture-pane`, `send-keys`, and
  `kill-server` succeed from separate client invocations.

### Phase 6: Interactive tmux Acceptance

Exercise attach/detach, pane lifecycle, resize, and job control over the
existing text console. This phase is the standalone deliverable; GUI work may
depend on it afterward.

Exit gate:

- Create two panes, send distinct commands, capture each pane, resize the
  text console, detach, reattach, and kill the server.
- A pane running `yes | head` or `sleep` responds to Ctrl-C and exits without
  leaking PTY slots, files, sockets, queued SCM_RIGHTS references, or children.
- Updating the outer text VT with `TIOCSWINSZ` delivers `SIGWINCH` to tmux and
  updates `stty size` in each pane; this does not require a GUI resize event.
- 50 create/detach/attach/kill cycles leave all 32 PTY slots allocatable.

## Test Matrix

### Kernel and libc Tests

Add focused native tests under `app/test/pty/` and shell wrappers under
`tests/shell/`:

- `pty.basic.test`: allocation, lock, `TIOCGPTN`, unlock, bidirectional I/O.
- `pty.libc.test`: Mario musl `posix_openpt`, `grantpt`, `unlockpt`,
  `ptsname_r`, `openpty`, and `forkpty`/`login_tty` end to end, including
  supplied termios and window-size arguments.
- `pty.termios.test`: canonical versus raw behavior, echo, `VINTR`, and EOF.
- `pty.flow-control.test`: fills each PTY direction, proves blocking and
  `O_NONBLOCK` short-write behavior, verifies no master-input or echo loss,
  exercises `TCSETSW`/`TCSETSF` drain/flush behavior, and confirms keyboard/
  serial adapters remain non-sleeping under input pressure.
- `pty.poll.test`: readable/writable transitions, full queues, nonblocking
  errors, master/slave close, exact EOF/`EIO` behavior, and `POLLHUP|POLLERR`.
  Its master-close cases cover both raw input and an unterminated canonical
  line, and require immediate EOF with no stale input delivery.
- `pty.job-control.test`: `setsid`, `TIOCSCTTY`, foreground pgrp, Ctrl-C,
  Ctrl-Z, `SIGTTIN`, `SIGTTOU`, `TIOCNOTTY`, and session-leader exit detach.
  The exit case keeps a child in the foreground pgrp, verifies one ordered
  `SIGHUP`/`SIGCONT` delivery, then proves the PTY slot is reusable only after
  the detached session state is no longer observable.
- `pty.current-tty.test`: `open("/dev/tty")` after automatic acquisition and
  after `TIOCSCTTY` on an inherited slave fd; verifies data, termios, pgrp,
  and window-size ioctls reach the PTY slave rather than a VT minor collision.
- `pty.winsize.test`: `TIOCSWINSZ`, `TIOCGWINSZ`, and `SIGWINCH`.
- `pty.lifetime.test`: `dup`, `fork`, `exec`, final `.release`, stale static
  slave nodes, deterministic lowest-slot reuse, and all 32-slot exhaustion.
  It also queues input, closes the final slave while retaining the master,
  reopens that slave, and verifies the queued input plus terminal/session state
  survive exactly until full PTY teardown.
- `unix.socket.test`: listen/connect/`accept4`/poll/close/stale-path/unlink.
- `unix.rights.test`: `socketpair`, `sendmsg`/`recvmsg`, one `SCM_RIGHTS` fd,
  receiver installation, plain-read/control-truncation/fd-table-full disposal,
  queue teardown, and rejection of unsupported cmsgs.
- `unix.shutdown.test`: full-backlog blocking/nonblocking connect, listener
  close, half-close EOF, `SHUT_*`, `SIGPIPE`/`MSG_NOSIGNAL`, queue bounds, and
  listener-versus-connected `poll` masks.

Each runs at `--smp 1` and `--smp 4`. The close/hangup, allocation/reuse, and
socket cleanup cases additionally run in a loop at `--smp 4 --jobs 4` to
catch descriptor/refcount races.

### tmux VGA Integration Test

Test interactive tmux through the existing `tests/lit_vga_test.py` harness,
just like `tests/vga/vim-1.test`; do not replace this with a serial-only shell
smoke test. `tests/vga/tmux-1.test` and its checked-in
`tests/vga/tmux-1.ppm` reference capture exercise the implemented baseline:

```text
VGA-SEND-KEY: f2
VGA-CHECK: / #
VGA-SEND: printf 'set -g status-right ""\n' > /tmp/tmux-vga.conf
VGA-SEND: tmux -L vga -f /tmp/tmux-vga.conf new-session -s vga /bin/oksh
SLEEP: 2
VGA-CHECK: [vga]
VGA-SEND: echo PANE_ONE
VGA-SEND-KEY: ctrl-b
VGA-SEND-RAW: %
SLEEP: 1
VGA-SEND: echo PANE_TWO
VGA-SEND-KEY: ctrl-b
VGA-SEND-RAW: "
SLEEP: 1
VGA-SEND: echo PANE_THREE
VGA-SEND-KEY: ctrl-b
VGA-SEND-RAW: c
SLEEP: 1
VGA-SEND: echo WINDOW_TWO
VGA-SEND-KEY: ctrl-b
VGA-SEND-RAW: p
SLEEP: 1
VGA-CHECK-NOT: server exited unexpectedly
VGA-CHECK-SCREEN: tmux-1.ppm
```

The test checks output from all three panes and from the second window before
returning to the split window. It waits after pane/window commands because
their redraws are asynchronous, then uses `VGA-CHECK-SCREEN` to compare the
whole tmux framebuffer at runtime. The local config removes only the dynamic
status-right clock; the status bar, pane borders, and pane contents remain in
the reference image. Keep the negative server-exit assertion: a server that
dies after a pane resize can otherwise leave stale pane text on the display.

Add separate noninteractive shell coverage for `list-sessions`, `capture-pane`,
`send-keys`, `kill-server`, stale-path cleanup, two-pane isolation, and outer
`TIOCSWINSZ` propagation. Those checks validate the client/server protocol;
the VGA test is the required end-user acceptance test.

## Failure Diagnostics

Add opt-in debug counters under `/sys/kernel/debug/pty`:

- allocated slots, master/slave endpoint-open counts, locked slaves, queued
  input and output bytes, master/slave final-close counts, hangups, poll
  wakeups, and blocked reader/writer counts;
- `cat` reports counters and active-slot summaries only;
- `echo reset >` resets counters only and never frees active slots or changes
  queue state.

When a test hangs, capture before killing QEMU:

1. `/sys/kernel/debug/pty` output;
2. `/sys/kernel/debug/syscalls` for the test pid when possible;
3. process/pgrp/session/tty state through a small diagnostic syscall or
   debugfs record; and
4. the final serial transcript.

Do not "fix" a PTY hang by increasing lit timeouts. Classify it as a queue,
endpoint-reference, session, signal, or socket-state failure first.

## Rollback and Compatibility

- Keep existing virtual consoles, serial consoles, anonymous pipes, and tty
  major/minor mappings unchanged.
- The PTY driver registers only its new major and does not alter `/dev/ttyN`
  behavior during Phase 2.
- If PTY initialization fails, omit `/dev/ptmx` functionality and boot the
  existing text environment; do not prevent init from starting.
- If AF_UNIX or tmux is incomplete, retain the PTY test helper and oksh-over-
  PTY workflow as a shippable intermediate milestone.
- Revert in small commits by phase: ramdisk manifest support, tty refactor,
  PTY driver, job control, Unix sockets/FD passing, libevent package, tmux
  package, and tests. Never combine all of them into one rollback unit.

## Final Acceptance Checklist

- `/dev/ptmx` allocates, locks, unlocks, and reuses `/dev/pts/N` correctly.
- A child shell owns a slave controlling tty and supports foreground job
  control without a GUI.
- Handoff, EOF, queue bounds, `poll`, resize, and hangup semantics pass at
  SMP=1 and SMP=4.
- The tmux-required `AF_UNIX/SOCK_STREAM`, `socketpair`, and one-FD
  `SCM_RIGHTS` contracts pass independent lifecycle tests.
- `libevent` and tmux are statically built with the Mario musl toolchain.
- `tmux` creates a server, spawns PTY-backed panes, sends/captures commands,
  detaches/attaches, resizes, and cleans up every resource.
- Existing tty, pipe, shell, and filesystem tests remain green.
