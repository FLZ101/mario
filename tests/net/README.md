# Network tests

Run `make net-test SMP=1` and `make net-test SMP=2` to exercise IPv4 TCP
and UDP without a NIC. The runner places the compiled fixtures on a
temporary ramdisk and requires a success marker and a zero exit status
from each fixture. The resolver fixture overrides `/etc/resolv.conf`
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
