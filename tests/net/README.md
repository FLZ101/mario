# Network tests

Run `make net-test SMP=1` and `make net-test SMP=2` to check TCP and UDP
loopback. The runner boots QEMU without a NIC and adds the compiled test
fixture on a temporary ramdisk rather than to the regular disk images.
It requires both the fixture's success marker and a zero exit status.

The loopback test covers musl, direct i386 syscalls, and legacy `socketcall`.
IPv4 fragmentation and reassembly are disabled: the 1500-byte MTU allows
UDP payloads of up to 1472 bytes. Larger datagrams fail with `EMSGSIZE`.
