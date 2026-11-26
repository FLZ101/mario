# Network tests

Run `make net-test SMP=1` and `make net-test SMP=2` to exercise IPv4 both with
and without QEMU's `e1000` device. The runner places the compiled fixtures on a
temporary ramdisk. The loopback runner requires a success marker and a zero
exit status from each fixture. The resolver fixture overrides `/etc/resolv.conf`
only inside the disposable QEMU snapshot and verifies UDP DNS and the
bundled musl resolver's TCP retry after a truncated reply.

The loopback test covers musl, direct i386 syscalls, and legacy `socketcall`.
IPv4 fragmentation and reassembly are disabled: the 1500-byte MTU allows
UDP payloads of up to 1472 bytes. Larger datagrams fail with `EMSGSIZE`.

Run the two-minute `TIME_WAIT` expiry check separately, after building the
normal test artifacts:

```sh
PYTHONPATH=tests .venv/bin/python tests/net/boot.py --smp 1 --time-wait
PYTHONPATH=tests .venv/bin/python tests/net/boot.py --smp 2 --time-wait
```

## Ethernet and static IPv4

The Ethernet tests require QEMU's `e1000` (82540EM, PCI `8086:100e`) and
were validated with QEMU 9.2.4. The
socket backend checks ARP, ICMP, UDP, static route selection, link and admin
transitions, and recovery from RX/TX ring pressure. A restricted user-mode
backend checks TCP over loopback-only forwards in both directions. A separate
relay drops the first TCP data segment between two guests and requires a
retransmission of the same bytes. No test uses a public peer or requires TAP.

The driver maps BAR0 at boot and allocates 14 DMA pages: two descriptor pages,
16 RX buffers, and 8 TX buffers of 2048 bytes each. It reserves a descriptor
in each ring to distinguish full from empty. IRQ10/IRQ11 share the existing
AHCI dispatch; the e1000 handler reads its own cause, masks interrupts, and
wakes the worker. The worker owns ring indices, drains at most 16 RX frames
per pass, batches TX doorbells, re-enables interrupts, and rechecks RX before
sleeping. DMA pages stay pinned through machine shutdown, when the worker
masks the NIC and disables RX/TX. There is no hot-plug or physical NIC support.

The normal image installs `/sbin/netctl.exe`. With an explicitly attached NIC,
`netctl.exe status` reports admin state, carrier, address source, and counters.
`netctl.exe up 192.0.2.2/24 192.0.2.1` configures a static address and an
optional on-link gateway; `netctl.exe down` disables routes but preserves
that configuration. The versioned `MARIO_NET_GET` and `MARIO_NET_SET` ioctls
run on an IPv4 socket; addresses are in network byte order, all other fields
are in host byte order, and the probe error is a positive errno. Changes use
Mario's effective-UID check; the current system reports UID 0 for all tasks.
