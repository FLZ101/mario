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

## QEMU network modes and DHCP

Build the kernel and apps (`make kernel app`), then choose `QEMU_NET` for
`make run-gtk` or any of the run/debug variants:

| Mode | Behavior |
| --- | --- |
| `off` | No virtual NIC. Default except for `run-gtk` and `debug-gtk`. |
| `isolated` | `e1000` with restricted QEMU user networking; no outside access or port forwards. |
| `nat` | `e1000` with outbound IPv4 TCP/UDP to hosts reachable from QEMU. Default for `run-gtk` and `debug-gtk`; no TAP setup needed. |

Use NAT for development testing on a trusted network. Mario does not yet
have hardened TCP randomness, so don't use this mode for sensitive traffic.
Check with an HTTP request to a known endpoint rather than relying on
Internet ping.

NAT forwards a host loopback port to port 22 at the guest's default DHCP
address. `QEMU_SSH_PORT=auto` (the default) tries port 2222 and falls back to
an OS-assigned port if it is busy. The launcher prints the bound address and
port at startup. Set `QEMU_SSH_PORT=2223` to require a particular port or
`QEMU_SSH_PORT=` to disable the forward. SSH login requires a guest SSH
server, which Mario does not yet ship.

In the guest, run `/sbin/netctl.exe dhcp start`, then check
`/sbin/netctl.exe dhcp status` for carrier, the leased address, gateway,
remaining lease time and nameservers. Acquisition retries in the background
and never delays boot. `dhcp stop`, `down`, or a static `up` cancels DHCP;
carrier loss drops the lease and carrier recovery starts a fresh discovery.
An infinite lease reports `lease_seconds_left=4294967295`.

DNS is manual by default: enabling DHCP alone never writes `/etc/resolv.conf`.
Run `/sbin/netctl.exe dns auto &` once to opt in to replacing that file.
It polls once per second, publishes changed nameservers by renaming a
completed file, and removes DHCP entries when the lease is lost. If a reader
holds the file open on MarioFS, the helper retries on the next poll. Identical
DNS renewals do not rewrite the file. Stop the process before editing it
manually again.

The DHCP test uses a scripted Ethernet peer to exercise silence, NAK,
renewal, route/DNS changes, expiry, link recovery and static override. The
Python test runs the installed guest interpreter with its native socket and
poll support. It checks TCP, UDP, musl DNS, HTTP in both directions and
`asyncio`, with test-only loopback forwards and no public servers.

## Python HTTPS

The installed Python links OpenSSL statically. `getrandom()` and
`/dev/urandom` require `RDRAND` from the CPU or a trusted hypervisor. They
fail with `ENODEV` if the feature is unavailable; Python normally needs a
working entropy source even at startup. Mario's QEMU launch targets use
`-cpu max`; the shell and network runners explicitly enable `RDRAND`.

The HTTPS test checks the guest UTC clock, generates a temporary test CA,
and checks exact payloads in both directions. Python verifies the explicitly
supplied CA and rejects a wrong hostname, an untrusted CA, and an expired
leaf. No public CA bundle is installed. A separate boot with `RDRAND`
disabled verifies that the random APIs and OpenSSL fail closed.
