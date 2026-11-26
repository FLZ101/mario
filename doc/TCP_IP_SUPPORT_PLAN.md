# TCP/IP support for Mario

## Scope

Run ordinary i386 musl applications over TCP and UDP using Mario file
descriptors. Start with `127.0.0.1`, then add one Ethernet interface in QEMU,
static IPv4 configuration, DHCP, and DNS. Verify guest Python sockets, HTTP,
and `asyncio` over isolated networking, including connections from a host
peer and certificate-verified HTTPS in userspace. Finish by enabling compatible
network tools in the bundled utility suites and shipping GNU Wget, curl, and
the supported net-tools utilities.
None of these tests requires public Internet access.

Keep the existing `AF_UNIX` behavior, including `SCM_RIGHTS` used by the GUI.
QEMU's `e1000` model (Intel 82540EM, PCI ID `8086:100e`) is the only Ethernet
target. Physical NICs, `e1000e`, and other Ethernet drivers are out of scope,
as are IPv6, raw sockets, multicast, VLANs, IP forwarding, firewalling, and
Wi-Fi. TLS belongs entirely in userspace. Release validation targets isolated
networks. QEMU outbound NAT is for development, not a production Internet
configuration.

## Stack and architecture

- **lwIP is the only stack in scope.** Build it separately as a freestanding
  i386 static archive, then link it into the Mario kernel. Keep imported
  sources, the build recipe, and the archive outside `src/` in a top-level
  `lwip/` directory, following Mario's `compiler-rt/` build pattern. Keep
  Mario-owned configuration (`lwipopts.h` and `arch/cc.h`), the network
  worker, socket adapter, and NIC driver under `src/net/`. Applications use
  musl sockets; they neither link nor install `liblwip.a`.
- Run lwIP's raw callback API (`NO_SYS=1`) on one Mario network worker, not
  on application threads or in IRQ context. Here `NO_SYS` means lwIP does
  not manage its own threads; the worker still runs under Mario's scheduler.
  The worker owns protocol state, interface configuration, and lwIP timers.
  It services bounded syscall, RX, and timer batches so traffic cannot
  starve close or retransmission. Queue loopback input and drain it in the
  worker rather than recursing into lwIP callbacks. Do not expose lwIP
  socket numbers to apps.
- Dispatch `AF_INET` to its own file operations; keep `AF_UNIX` behavior and
  reject unsupported families or options explicitly. Share syscall plumbing,
  not endpoint state. Implement the existing i386 `socketcall` ABI and the
  direct syscall entries against the same handlers. Validate IPv4 address and
  length arguments and preserve network byte order.
- Keep user pointers out of worker requests. Stage bounded copies outside
  spinlocks; IRQ handlers only acknowledge device events and signal the
  worker. The worker never waits on a socket condition or touches userspace.
  Publish readiness and register waits under the same synchronization,
  rechecking the condition before sleeping so a completion cannot be lost.
  Keep the lock order and ownership rules in the port documentation.

## Milestones

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| N0 — Build and worker | Separate `liblwip.a` linked into the kernel; one worker with timers | Freestanding build and offline QEMU boot; the worker starts on SMP=1 and SMP=2 without a NIC. |
| N1 — Loopback sockets | Native IPv4 TCP and UDP file descriptors | Unmodified musl socket calls exchange exact bytes over loopback, without a NIC. |
| N2 — Socket semantics | Polling, interruption, backpressure, teardown | The full i386 socket syscall matrix passes; the stock musl resolver works, including TCP fallback; AF_UNIX regressions pass. |
| N3 — Ethernet | Intel 82540EM in QEMU (`e1000`) and static addressing | Isolated ARP, ICMP, and UDP fixtures pass; TCP works in both directions and the driver recovers from ring pressure and link changes. |
| N4 — Configuration and Python networking | DHCP, DNS, run targets, and guest Python socket, HTTP, and `asyncio` support | Lease renewal, expiry, and static override work; guest Python passes sync and async socket tests and fetches the HTTP fixture by hostname; a host client reaches Python's HTTP server. |
| N5 — Python HTTPS | Guest Python `ssl` and a userspace TLS library | Guest and host HTTPS clients verify certificates and fetch the exact payload; invalid trust and hostname checks fail. |
| N6 — Network tools | Compatible Toybox, ubase, sbase, and net-tools utilities, standalone GNU Wget and curl | Installed network utilities pass their isolated smoke tests; both standalone clients fetch the exact HTTP and HTTPS fixtures and reject invalid certificates. |

### N0 — Build and worker

1. Use the verified, vendored upstream sources in `lwip/src/`, like
   `compiler-rt`. Record the release tag, archive SHA-256, and BSD notices
   in `lwip/`. Build a fixed upstream source list with `make lwip`,
   `util/mario-cc`, the kernel's freestanding ABI flags, and the port headers
   under `src/net/`. Produce `lwip/build/liblwip.a` without musl or host libc.
   Do not add upstream sources to the kernel's source glob.
2. Build and link the archive into every kernel, after the Mario objects.
   Rebuild the archive when the pinned source, port headers, or compiler
   flags change; relink the kernel when the archive changes. Follow the same
   native and container build paths as `compiler-rt`, without fetching
   sources. Verify ELF32 i386 objects and no host libc or floating-point
   runtime dependencies.
3. Supply lwIP's platform headers, bounded allocation and logging hooks,
   and a wrapping 32-bit monotonic millisecond clock. Run
   `sys_check_timeouts()` on the worker and sleep until pending work or the
   next lwIP deadline. Test sustained traffic and concurrent work
   submission in N1–N3.
4. Verify boot without Ethernet. Until N1 adds IPv4 sockets, reject `AF_INET`
   with `EAFNOSUPPORT` without affecting `AF_UNIX`. Keep applications
   independent of lwIP headers.

### N1 — Loopback sockets

1. Add `AF_INET` TCP `socket`, `bind`, `listen`, `connect`, `accept4`,
   `shutdown`, `read`/`write`, `getsockname`, and `getpeername`. Wire the
   missing direct i386 entries (`getsockname`, `getpeername`, `sendto`) and
   ensure the legacy `socketcall` operations reach the same implementation.
   Verify wildcard binds, local port selection, and the actual local and
   peer address and length returned to userspace.
2. Add UDP `bind`, `connect`, `sendto`, and `recvfrom`, including source
   addresses, zero-length datagrams, and datagram boundaries. A short receive
   consumes only the current datagram and reports truncation where requested.
   Do not treat a zero-length UDP datagram as TCP EOF. UDP sends are atomic;
   document and test the datagram size limit. Initially use a 1500-byte IPv4
   MTU with no IP fragmentation or reassembly, returning `EMSGSIZE` for
   payloads over 1472 bytes.
3. Keep the guest fixture sources and scenarios under `tests/net/`.
   Exercise both direct syscalls and legacy `socketcall`,
   along with normal musl wrappers. Verify TCP half-close, refused connection,
   occupied port, valid blocking accept, and UDP exchange without an Ethernet
   device. Include invalid address lengths, unsupported family/protocol
   combinations, and bad user pointers; errors must not leak sockets or
   change a successful bind.

### N2 — Socket semantics and limits

1. Implement `O_NONBLOCK`, `SOCK_NONBLOCK`/`SOCK_CLOEXEC`, `accept4` flags,
   short stream I/O, `MSG_DONTWAIT`, signal interruption, and
   `SIGPIPE`/`MSG_NOSIGNAL`. Define
   `poll()` readiness for accept, data, capacity, EOF, and errors; verify
   both `poll()` and musl `select()`. Test `EINPROGRESS`, a second connect
   while pending (`EALREADY`), and completion or failure through
   `SO_ERROR` (read and clear), not writability alone. Check that unread
   data is delivered before orderly EOF and that TCP half-close preserves
   the other direction.
2. Implement the IPv4 data path for `sendmsg`/`recvmsg`, including iovecs,
   peer addresses and `MSG_TRUNC` (both the returned flag and the input flag
   requesting the full datagram length); do not share Unix descriptor passing.
   Start with `SO_ERROR`, `SO_REUSEADDR`, `SO_TYPE`, and `TCP_NODELAY`.
   Unsupported options return `ENOPROTOOPT`. In particular, leave
   `TCP_FASTOPEN_CONNECT` unsupported so the bundled musl resolver can fall
   back to ordinary `connect()` and scatter/gather I/O. Run `getaddrinfo()`
   against a loopback DNS fixture that serves both normal and truncated UDP
   replies and answers the TCP retry, without patching musl.
3. Separate open-file, queued-operation, and protocol references. Last close
   detaches the socket from userspace and schedules worker teardown; an
   in-flight syscall or late lwIP callback must not touch freed state or a
   recycled fd number. Define the cancellation boundary: cancel before an
   operation commits, return progress if it already committed, and detach an
   interrupted waiter from asynchronous completion with no user pointer
   retained. Verify `dup`/`fork`, close versus in-flight work, interrupted
   waiters, and wakeups while another thread changes readiness.
4. Return TCP receive credit (`tcp_recved`) only when receive capacity is
   freed. If the receive queue is full, apply TCP backpressure and drop
   whole UDP datagrams with a counter. Bound stream staging and make
   nonblocking operations return `EAGAIN` when no progress is possible.
   Reserve a close/control path even when data queues are full; handle a
   retryable lwIP close and a bounded abort if it cannot complete.
5. Publish the initial limits and worst-case memory budget before N2 signoff:
   live and closing sockets, per-listener backlog, per-socket stream bytes
   and UDP datagrams, lwIP pools, and worker queues. Test each configured
   limit with small test settings and verify error delivery, continued
   progress for unrelated sockets, and reclamation after normal `TIME_WAIT`.
   Run these cases on SMP=1 and SMP=2, plus the AF_UNIX socket/`recvfrom`
   tests and a desktop launch smoke test for descriptor passing. Run Mario's
   kernel lock checker and build at each kernel milestone.

#### Initial socket and memory limits

- At most 48 IPv4 endpoints, including descriptors with a failed connection,
  queued accepts, and endpoints waiting to finish a close. lwIP has 24 TCP
  connection PCBs (including `TIME_WAIT`), 8 listen PCBs, and 16 UDP PCBs.
  A listener queues up to 8 accepts, subject to the PCB and endpoint limits.
- TCP grants 2144 bytes of receive window and 1072 bytes of send buffer per
  connection. The receive queue holds at most 8 pbuf chains; a full TCP
  queue refuses the next pbuf until lwIP can retry. The UDP queue holds at
  most 8 datagrams (up to 11,776 bytes of payload), further bounded by the
  shared lwIP heap. Full UDP queues drop the entire next datagram and
  increment `udp_rx_drops` in `/sys/kernel/debug/net`.
- At most 16 staged reads and 16 staged writes, plus 4 nonblocking reserve
  slots per direction. Staging holds at most 4096 bytes of stream data (1472
  for UDP) and at most 1024 iovecs per call. With i386 allocator rounding,
  all 40 slots together use at most 960 KiB of allocator pages. Each endpoint
  fits in a 512-byte allocation. lwIP uses a 32 KiB heap, 64 TCP segments,
  32 pool pbufs, 16 pbuf headers, and one timeout slot beyond its internal
  timers; the i386 heap and pools occupy less than 58 KiB of static storage.
- The worker accepts up to 64 queued synchronous requests. Close work uses an
  embedded item per endpoint and does not consume a synchronous queue slot.
  Last close schedules teardown without sleeping; if lwIP defers FIN for
  lack of memory, the worker lets it retry and aborts after roughly 2 seconds
  if FIN is still pending. A normal close uses lwIP's ordinary `TIME_WAIT`.

A syscall holds a file reference for the full operation, including poll
registration. The network worker sees only kernel-owned data. A signal can
remove an unclaimed request from the worker queue; after the worker claims
it, the caller waits for the result and returns any committed progress. An
interrupted blocked connect leaves the TCP handshake in progress, but its
PCB no longer depends on any caller-owned buffer or wait node.

#### i386 socket syscall test matrix

Drive the raw guest ABI from a C fixture so musl's fallback cannot hide an
unimplemented entry. Run each applicable case through both direct and
`socketcall` entries, and smoke-test the public musl wrappers separately.
`accept`, `send`, and `recv` have legacy entries only; their libc wrappers
must also work. Test every `socketcall` operation (1–20), direct socket call
(359–373), and direct batched-message entry (337, 345, 417).

| Calls | Required cases |
| --- | --- |
| `socket` | IPv4 TCP/UDP with protocol zero and explicit protocol; nonblocking and close-on-exec flags; invalid type/flags, incompatible protocol, unsupported family. |
| `socketpair` | AF_UNIX stream pair with bidirectional exact bytes and flags on both ends; AF_INET rejection; bad result pointer without leaking descriptors. |
| `bind` | Wildcard and loopback addresses, ephemeral port, duplicate address/port, unavailable address, invalid family and length. |
| `listen` | Valid TCP listener and bounded backlog, reject UDP and connected sockets, verify backlog recovery after draining. |
| `connect` | TCP success/refusal and nonblocking completion via `poll`/`SO_ERROR`; pending and already-connected cases; UDP default peer and `AF_UNSPEC` disconnect. |
| `accept`, `accept4` | Blocking and nonblocking accept, peer address and length, omitted peer address, both `accept4` flags and invalid flags; interrupt and backlog recovery. |
| `getsockname`, `getpeername` | Wildcard bind and selected local port/address; correct connection endpoints, unconnected peer error, short output buffer and returned full address length. |
| `setsockopt`, `getsockopt` | Set/read `SO_REUSEADDR` and `TCP_NODELAY`; read `SO_TYPE`; consume and clear pending `SO_ERROR`; reject unsupported levels/options and invalid lengths. |
| `send`, `sendto` | Connected TCP/UDP, unconnected UDP to an explicit peer, zero-length/oversized datagrams, failed nonblocking send, and flag handling (`MSG_DONTWAIT`, `MSG_NOSIGNAL`). Ensure legacy `sendto` uses the destination and flags. |
| `recv`, `recvfrom` | TCP short reads and EOF, UDP source address and length, zero-length datagrams, short buffer without spilling into the next datagram, `MSG_PEEK`, and `MSG_DONTWAIT`. |
| `sendmsg`, `recvmsg` | Multi-iovec stream and datagram I/O, peer metadata, `MSG_TRUNC` input and output, invalid iovec/count/length, and rejection of `SCM_RIGHTS` on AF_INET. Verify AF_UNIX descriptor passing still works. |
| `shutdown` | `SHUT_RD`, `SHUT_WR`, `SHUT_RDWR`, half-close and EOF after queued bytes, writes after shutdown, an unconnected socket, and an invalid `how`. |
| `sendmmsg`, `recvmmsg`, `recvmmsg_time64` | Batched I/O is deferred; assert `ENOSYS` through the direct entries, including the time64 receive entry, and the two legacy socketcall operations. |
| `socketcall` dispatch | Reject unknown operations and fault on an invalid argument vector for supported operations; no libc fallback in these tests. |

Across the supported calls, exercise closed descriptors, non-socket
descriptors, invalid user pointers and lengths where applicable, and assert
the expected errno and that failed calls do not leak descriptors or buffers.
Cover `read`/`write`, `fcntl`, `poll`/`select`, `dup`, `fork`, and `close` in
the same fixture where they affect socket behavior and descriptor lifetime.

### N3 — Ethernet and static IPv4

1. Use QEMU's `e1000` model (Intel 82540EM, `8086:100e`). Match only that
   PCI ID. Check `qemu-system-i386 -device help` before running network
   tests and give a clear error if `e1000` is missing. Reuse Mario's PCI
   discovery and MMIO mapping. Confirm the selected QEMU machine's PCI
   INTx route under SMP and add a small dispatch hook to Mario's existing
   IRQ path as needed; do not assume there is a driver IRQ registration API.
   Prove reset, MAC read, and one frame in each direction with polling as
   a bring-up step before enabling interrupts.
2. Use one RX/TX queue pair, fixed-size DMA rings backed by memory below
   4 GiB, legacy INTx interrupts, a 1500-byte MTU, and software checksums.
   Start with copy-based buffers: pass DMA addresses, not kernel virtual
   addresses, to the device; use the required alignment and DMA ordering
   before updating tails and after observing completion. Reclaim TX only
   after completion, and reject incomplete, errored, or oversized RX frames
   before handing bytes to lwIP. Keep reset, PHY, and ring ownership in the
   driver. Skip hardware offloads, jumbo frames, and power management.
3. Keep INTx handling bounded: check the device's cause before claiming a
   shared interrupt, acknowledge and mask device events as needed, and wake
   the worker. Before sleeping after a drain, recheck pending events and
   descriptors as interrupts are re-enabled. For failure or shutdown, mask
   the device, synchronize IRQ/worker access, and stop DMA before freeing
   rings. If probe fails, leave `lo` functional and report the failure once.
4. Register `eth0` alongside `lo`. Provide a small `netctl` using versioned
   Mario ioctls on an `AF_INET` socket. Use fixed-width request fields and
   keep user copies in the syscall path. Expose status without privilege;
   require root for admin state, one unicast IPv4 address/prefix, and one
   optional on-link default gateway.
   Reject invalid combinations without changing the current config. Keep
   administrative state and carrier state distinct; a static config survives
   link loss, but its routes are unusable while down. Verify loopback, on-link,
   and default-route selection, including ARP for the gateway (not the
   off-link destination). Report link, configuration source, packet/drop
   counts, queue high-water marks, and probe failures. Linux network tools
   and a general routing table are not part of this ABI.
5. Test the NIC independently of QEMU's NAT. A host-side Ethernet fixture
   over QEMU's socket backend checks ARP, ICMP echo, UDP in both directions,
   and static routing. Use restricted QEMU user networking with explicit
   loopback forwards for TCP in both directions, without implementing a TCP
   stack in the host fixture. Transfer exact bytes across many ring wraps and
   drive RX exhaustion and TX backpressure with small rings. Toggle carrier
   via QMP and verify both directions work again without reboot. Exercise
   deterministic TCP loss/retransmission on the isolated link, for example
   by dropping a selected data frame in a relay between two test guests.

### N4 — DHCP, DNS, and Python networking

1. Add asynchronous DHCP start/status/stop through the same control ABI.
   The worker owns a validated lease snapshot: apply the address and route
   together and publish the associated DNS servers with a generation number.
   Start acquisition when administratively enabled and carrier is up; never
   wait for DHCP at boot. Clear DHCP-owned settings on expiry, stop, or link
   loss, and reacquire on link-up. Switching to static cancels DHCP and stale
   callbacks must not overwrite the manual config. With a scripted DHCP peer,
   verify a successful renewal extends the lease, changed route/DNS settings
   take effect, and a silent or rejecting server leaves `lo` working and
   permits manual configuration.
2. Keep hostname resolution in musl. In automatic DNS mode a privileged
   userspace helper polls lease generations at a bounded interval and
   atomically replaces `/etc/resolv.conf`, including removing old DHCP
   nameservers when the lease disappears. A renewal must not rewrite the
   file if the DNS list is unchanged. Manual mode never overwrites the
   user's resolver file. Test these transitions and verify a DNS query
   actually reaches the new server after a lease change. The TCP fallback
   remains covered by N2.
3. Provide a reusable host HTTP fixture via restricted `guestfwd` and a
   guest-local fixture DNS server, so clients can test both numeric and
   DNS-resolved addresses without `/etc/hosts`. Test DHCP-provided DNS
   separately on the scripted Ethernet link. Use loopback-only `hostfwd`
   for inbound tests; do not enable a listener or port forwards in
   distributed images.
4. Offer `QEMU_NET=isolated` for restricted developer runs without port
   forwards, `QEMU_NET=nat` for outbound IPv4, and `QEMU_NET=off` for no NIC.
   Default to NAT for `run-gtk` and `debug-gtk` and no NIC for other launch
   targets. In NAT mode, forward host loopback to guest TCP port 22. Prefer
   port 2222, fall back to an OS-assigned port, and report the bound port at
   launch. `QEMU_SSH_PORT` can fix the port or disable the forward. Document
   how to build, enable, configure, and diagnose the NIC. Keep fixed seeds in
   test fixtures only. Before treating external connectivity as production
   ready, require a trusted entropy source (N5), keyed TCP initial sequence
   numbers, and unpredictable ephemeral port selection.
5. Enable guest Python's `_socket` in `app/usr/python.Setup.local` and
   remove the forced `py_cv_module__socket=no` in `app/usr/build-python.sh`.
   Retain `socket`, `http`, `email`, `urllib.request`, `selectors`, and
   `asyncio` with their dependencies in the packaged standard library.
   Verify required imports in the installed Mario interpreter. Use
   `selectors.DefaultSelector` with a supported backend; do not advertise
   guest `epoll` while the kernel lacks it.
6. Add guest Python `socket` cases under `tests/net/`: TCP echo with
   `sendall`, timeouts, half-close, and nonblocking readiness; UDP echo,
   peer addresses, and datagram boundaries; and `getaddrinfo()` success
   and failure against the fixture DNS server.
7. Use `http.client` to test the host HTTP fixture by IP and `urllib.request`
   to fetch it by hostname. Verify a guest `http.server` from a host client.
   Check status, headers, and exact binary bodies, plus a 404, a refused
   connection, and a bounded read timeout. Disable proxy use in the fixture
   clients.
8. Add `asyncio` TCP streams and UDP datagram endpoint tests for bidirectional
   data transfer, readiness, and cleanup. Resolve the TCP peer with the
   event loop's `getaddrinfo()`. Exercise a timed-out read and cancellation
   of a pending read, then verify the loop can still serve another connection
   and all test tasks and transports close before shutdown.

### N5 — Python HTTPS

1. Build a pinned OpenSSL release for i386 Mario through the normal app
   build and link Python's `_ssl` against the target libraries. Enable the
   currently disabled `_ssl` module and verify `import ssl` in the shipped
   guest interpreter. Keep TLS out of the kernel and lwIP; host SSL support
   does not count as a guest pass.
2. Feed `getrandom()` and `/dev/urandom` from CPU `RDRAND`, which must be
   advertised by CPUID. Use only values with the success flag set. Retry
   transient failures a bounded number of times; fail closed if entropy is
   unavailable, including on CPUs without `RDRAND`. Verify that OpenSSL
   cannot seed when QEMU hides the CPU feature. Confirm the guest UTC clock
   for certificate verification. Keep the tests on the isolated link; TLS
   does not change the default QEMU network mode or harden lwIP's port and
   TCP sequence number selection.
3. Generate a test CA and server certificates with matching DNS SANs in
   the host fixture and keep keys in test-only temporary files. Pass the
   CA explicitly to a verifying `ssl.SSLContext`; do not change system
   trust stores or disable hostname verification. Resolve the fixture name
   via isolated DNS and fetch the exact payload from guest Python using
   `http.client.HTTPSConnection`. Also wrap the guest HTTP server with a
   server-side TLS context and have a host client verify it. Assert explicit
   verification failures for a wrong hostname, an untrusted CA, and an
   expired leaf.

### N6 — Network tools

1. Audit the pinned Toybox, ubase, and sbase sources and Mario build configs
   for network applets and utilities. Enable and install the ones compatible
   with the shipped IPv4 socket and interface-control APIs. Record the result
   for each suite, including absent tools and blockers such as raw sockets,
   IPv6, or Linux-specific control APIs. Do not widen kernel scope just to
   enable an applet.
2. Build pinned standalone GNU Wget and curl for i386 musl through the
   existing `app/usr` build and image rules. Reuse the target OpenSSL from
   N5, enable HTTP and HTTPS, and disable optional protocols and dependencies
   that are outside this release. Install `wget` and `curl` in the normal
   image. If Toybox also provides `wget`, retain it as `toybox wget` without
   letting a multicall symlink overwrite the standalone binary.
3. Add `tests/net/` smoke tests for each enabled utility using local peers.
   Run the installed standalone clients against the shared HTTP and HTTPS
   fixtures, checking exit statuses and exact downloaded bytes. Exercise
   HTTP by numeric address and DNS hostname, and verify HTTPS by hostname
   with the fixture CA supplied explicitly. Check nonzero exits for DNS
   failure, connection refusal, an untrusted CA, and a wrong hostname.
4. Build a pinned net-tools release through `app/usr`. Audit its ioctl and
   `/proc/net` dependencies against Mario and port the useful IPv4 subset
   (`ifconfig`, `route`, `netstat`, and `arp`) using supported interfaces.
   Only install utilities and modes covered by isolated guest tests; report
   unsupported operations rather than showing empty or misleading output.

## Test harness and signoff

- Keep all new networking scenarios, guest C and Python sources, host
  fixtures, and test assets under `tests/net/`. Compile the C fixture with
  the normal Mario musl app toolchain and stage fixtures in a test-only
  image; do not install them in distributed images. Keep the pre-existing
  shell and GUI regression suites in place.
- Include the `tests/net/` suite in `make test` and expose it separately as
  `make net-test`. Require Python socket, HTTP, `asyncio`, and HTTPS tests and
  utility smoke tests once their milestones land. Missing guest modules or
  required binaries are failures, not skips.
  Run host fixtures with the repository venv. Keep existing shell, TTY, and
  GUI tests on `-nic none`; only Ethernet cases instantiate `e1000`. A
  missing required QEMU backend or model is a setup failure, not a skipped
  pass.
- Use per-test MACs, loopback-only endpoints, and ephemeral ports for
  parallel runs. No TAP privileges, public DNS, or external network access.
  Put deadlines on boot, peer startup, and each protocol exchange; on a
  mismatch report the stage, guest serial output, and NIC/stack counters,
  then clean up every guest, peer, and socket on both success and failure.
- Sign off on SMP=1 and SMP=2, with and without `e1000`, plus the existing
  AF_UNIX regressions. Record the pinned lwIP
  version, tested QEMU version, configured memory limits, and exact test
  commands. Build without re-fetching sources.

## Alternatives considered

These are design notes, not additional implementation work. The milestones
use only lwIP, built separately and linked into the kernel.

### Other ways to integrate lwIP

| Approach | Trade-off for Mario |
| --- | --- |
| Compile the upstream sources directly under `src/` | One build graph and no archive step, but imported code would share Mario's source tree and build rules. The separate archive keeps the upstream source list, upgrades, and third-party flags distinct. Both run in kernel space. |
| Use lwIP's OS mode (`NO_SYS=0`) | Provides `netconn` and lwIP's own socket API, but requires a Mario `sys_arch` port and still needs an adapter for Mario descriptors, polling, signals, and close. Revisit in N0 only if it measurably simplifies that adapter. |
| Run lwIP in a userspace network service | Keeps protocol parsing outside the kernel, but transparent musl sockets would still need a kernel IPC bridge for waits, readiness, descriptor lifetime, and service failure. It also needs a way for the service to access NIC queues. Defer that split. |

### Other TCP/IP stacks

| Stack | Why it was not selected |
| --- | --- |
| [FNET](https://fnet.sourceforge.io/fnet_overview.html) | Embedded C stack, Apache 2.0, with a nonblocking socket API. Mario would still need a platform port and would own blocking calls and readiness. No clear integration benefit over lwIP for this plan. |
| [smoltcp](https://github.com/smoltcp-rs/smoltcp) | 0BSD Rust stack that can run without heap allocation. It would add a freestanding i386 Rust toolchain, a C interface, and a Mario socket adapter. |
| [picoTCP-NG](https://github.com/virtualsquare/picotcp) | Modular embedded C stack. A kernel build would need a GPLv2/v3 licensing review and review of upstream's security notes, in addition to the socket adapter. |
| [NetBSD rump networking](https://www.netbsd.org/docs/rump/sysproxy.html) | Reuses the NetBSD TCP/IP implementation but adds a rump runtime and a socket bridge. This is a broader port than Mario needs for IPv4 TCP and UDP. |

## References

- [lwIP source and license](https://github.com/lwip-tcpip/lwip) and
  [mainloop mode](https://www.nongnu.org/lwip/2_1_x/group__lwip__opts__nosys.html),
  [raw TCP API](https://www.nongnu.org/lwip/2_1_x/group__tcp__raw.html)
- [QEMU network device and backend options](https://www.qemu.org/docs/master/system/invocation.html)
- [QEMU e1000 model](https://github.com/qemu/qemu/blob/master/hw/net/e1000.c)
  and [Intel 82540EM specifications](https://www.intel.com/content/www/us/en/products/sku/1285/intel-82540em-gigabit-ethernet-controller/specifications.html)
