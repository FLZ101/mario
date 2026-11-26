"""Test DHCP lease transitions and musl DNS against an isolated Ethernet peer."""

import re
import select
import socket
import struct
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing
from dataclasses import dataclass
from threading import Event

from dns_fixture import answer
from ethernet import (
    BROADCAST,
    GUEST,
    GUEST_MAC,
    PEER,
    arp_frame,
    guest,
    ip_frame,
    is_arp,
    listener,
    packet,
    receive_frame,
    run,
    status,
    wait_status,
)
from lit_gui_test import Qmp

NETCTL = "/sbin/netctl.exe"
FIRST_DNS = "192.0.2.1"
SECOND_DNS = "198.51.100.53"
SECOND_ROUTER = "192.0.2.254"
FIXTURE_NAME = "leased.mario.test"


@dataclass(frozen=True)
class Lease:
    seconds: int = 16
    renew: int = 6
    dns: str = FIRST_DNS
    router: str = FIRST_DNS


def options(payload):
    assert payload[236:240] == b"\x63\x82\x53\x63", payload.hex()
    entries = {}
    offset = 240
    while offset < len(payload) and payload[offset] != 255:
        code = payload[offset]
        offset += 1
        if code:
            size = payload[offset]
            offset += 1
            entries[code] = payload[offset : offset + size]
            offset += size
    return entries


def reply(request, kind, lease):
    bootp = bytearray(240)
    bootp[0:4] = bytes((2, 1, 6, 0))
    bootp[4:8] = request[4:8]
    bootp[16:20] = GUEST if kind != 6 else b"\0" * 4
    bootp[20:24] = PEER
    bootp[28:34] = request[28:34]
    bootp[236:240] = request[236:240]
    entries = [(53, bytes((kind,))), (54, PEER)]
    if kind != 6:
        entries += [
            (1, b"\xff\xff\xff\0"),
            (3, socket.inet_aton(lease.router)),
            (6, socket.inet_aton(lease.dns)),
            (51, struct.pack("!I", lease.seconds)),
            (58, struct.pack("!I", lease.renew)),
            (59, struct.pack("!I", lease.seconds - 2)),
        ]
    for code, value in entries:
        bootp += bytes((code, len(value))) + value
    bootp += b"\xff"
    return ip_frame(17, udp(67, 68, bootp), PEER, BROADCAST[:4], BROADCAST)


def udp(source, destination, data):
    return struct.pack("!HHHH", source, destination, len(data) + 8, 0) + data


class Peer:
    def __init__(self):
        self.lease = None
        self.reject_next = False
        self.discovered = Event()
        self.rejected = Event()
        self.last_ack = None
        self.dns_queries = []
        self.arp_queries = []

    def respond(self, frame):
        for name in (FIRST_DNS, SECOND_ROUTER):
            address = socket.inet_aton(name)
            if is_arp(frame, 1, address) and frame[28:32] == GUEST:
                self.arp_queries.append(name)
                return arp_frame(2, GUEST_MAC, GUEST, address)
        if frame[12:14] != b"\x08\x00" or frame[23] != 17:
            return None
        offset = 14 + (frame[14] & 15) * 4
        source, destination, length = struct.unpack_from("!HHH", frame, offset)
        payload = frame[offset + 8 : offset + length]
        if destination == 53:
            addr = socket.inet_ntoa(frame[30:34])
            assert addr in (FIRST_DNS, SECOND_DNS), addr
            expected = "198.51.100.2" if addr == FIRST_DNS else "198.51.100.3"
            name, data = answer(payload, {FIXTURE_NAME: expected})
            self.dns_queries.append((addr, name))
            return ip_frame(17, udp(53, source, data), frame[30:34])
        if (source, destination) != (68, 67):
            return None
        assert payload[28:34] == GUEST_MAC, payload.hex()
        entries = options(payload)
        (kind,) = entries[53]
        lease = self.lease
        if kind == 1:
            assert 6 in entries[55], entries
            self.discovered.set()
            return reply(payload, 2, lease) if lease else None
        if kind == 3 and lease:
            if self.reject_next:
                self.reject_next = False
                self.lease = None
                self.rejected.set()
                return reply(payload, 6, lease)
            self.last_ack = reply(payload, 5, lease)
            return self.last_ack
        return None

    def serve(self, connection, done):
        connection.settimeout(2)
        while not done.is_set():
            if not select.select((connection,), (), (), 0.05)[0]:
                continue
            response = self.respond(receive_frame(connection))
            if response:
                connection.sendall(packet(response))


def sequence(output, field):
    return int(re.search(rf"\b{field}=(\d+)", output)[1])


def resolver(child, expected):
    deadline = time.monotonic() + 4
    while True:
        output = run(child, "cat /etc/resolv.conf")
        lines = output.replace("\r", "").splitlines()
        contents = [line for line in lines if line.startswith("nameserver ")]
        if contents == ([f"nameserver {expected}"] if expected else []):
            return lines
        if time.monotonic() >= deadline:
            raise AssertionError(output)
        time.sleep(0.1)


def lookup(child, expected):
    run(child, f"/mnt/resolver {FIXTURE_NAME} {expected}")


def check_dhcp(smp, directory, initrd):
    peer = Peer()
    with listener() as wire:
        backend = f"socket,id=wire,connect=127.0.0.1:{wire.getsockname()[1]}"
        with guest(smp, directory, initrd, backend) as (child, monitor):
            connection, _address = wire.accept()
            with connection, ThreadPoolExecutor(1) as pool, closing(
                Qmp(str(monitor))
            ) as qmp:
                done = Event()
                future = pool.submit(peer.serve, connection, done)
                try:
                    qmp.connect(timeout=5)
                    transitions(child, qmp, peer, connection)
                finally:
                    done.set()
                    future.result(timeout=3)


def transitions(child, qmp, peer, connection):
    wait_status(child, r"eth0: down carrier=up source=none")
    run(child, "printf 'nameserver 192.0.2.9\\n' >/etc/resolv.conf")
    manual = resolver(child, "192.0.2.9")
    run(child, f"{NETCTL} dhcp start")
    assert peer.discovered.wait(4), "DHCP did not discover"
    wait_status(child, r"address=0.0.0.0/0.*\r*\ndhcp=waiting")
    run(child, "/mnt/loopback")
    peer.reject_next = True
    peer.lease = Lease()
    assert peer.rejected.wait(8), "DHCP peer did not send a NAK"
    run(child, f"{NETCTL} up 192.0.2.2/24 {FIRST_DNS}")
    wait_status(child, r"source=static.*\r*\ndhcp=off")
    assert resolver(child, "192.0.2.9") == manual

    peer.lease = Lease()
    run(child, f"{NETCTL} dhcp start")
    first = wait_status(
        child,
        r"source=dhcp address=192\.0\.2\.2/24 gateway=192\.0\.2\.1.*\r*\ndhcp=bound",
        12,
    )
    first_generation = sequence(first, "lease_generation")
    dns_generation = sequence(first, "dns_generation")
    peer.lease = Lease(seconds=24)
    assert "dns=192.0.2.1" in first, first
    assert resolver(child, "192.0.2.9") == manual
    run(child, f"{NETCTL} dns auto & dns_pid=$!")
    resolver(child, FIRST_DNS)
    run(child, "printf '# preserve identical renewal\\n' >> /etc/resolv.conf")
    lookup(child, "198.51.100.2")

    renewed = wait_status(
        child, rf"dhcp=bound lease_generation={first_generation + 1}\b", 9
    )
    assert sequence(renewed, "lease_seconds_left") > sequence(
        first, "lease_seconds_left"
    ), renewed
    assert sequence(renewed, "dns_generation") == dns_generation, renewed
    time.sleep(1.1)
    assert "# preserve identical renewal" in resolver(child, FIRST_DNS)

    run(child, "exec 9</etc/resolv.conf")
    peer.lease = Lease(seconds=12, renew=4, dns=SECOND_DNS, router=SECOND_ROUTER)
    changed = wait_status(
        child,
        rf"gateway={re.escape(SECOND_ROUTER)}.*\r*\ndhcp=bound.*"
        rf"dns_generation={dns_generation + 1} dns={re.escape(SECOND_DNS)}",
        9,
    )
    assert sequence(changed, "lease_generation") == first_generation + 2, changed
    peer.lease = None
    time.sleep(1.1)
    assert "# preserve identical renewal" in resolver(child, FIRST_DNS)
    run(child, "exec 9<&-")
    assert "# preserve identical renewal" not in resolver(child, SECOND_DNS)
    lookup(child, "198.51.100.3")
    assert peer.dns_queries[-2:] == [
        (FIRST_DNS, FIXTURE_NAME),
        (SECOND_DNS, FIXTURE_NAME),
    ]
    assert SECOND_ROUTER in peer.arp_queries, peer.arp_queries
    wait_status(
        child, r"address=0\.0\.0\.0/0 gateway=0\.0\.0\.0.*\r*\ndhcp=waiting", 14
    )
    resolver(child, None)

    peer.lease = Lease(seconds=24)
    qmp.command("set_link", {"name": "ethernet", "up": False})
    wait_status(child, r"eth0: up carrier=down source=dhcp")
    qmp.command("set_link", {"name": "ethernet", "up": True})
    wait_status(child, r"eth0: up carrier=up source=dhcp.*\r*\ndhcp=bound", 12)
    resolver(child, FIRST_DNS)
    qmp.command("set_link", {"name": "ethernet", "up": False})
    wait_status(
        child,
        r"eth0: up carrier=down source=dhcp address=0\.0\.0\.0/0.*\r*\ndhcp=waiting",
    )
    resolver(child, None)
    qmp.command("set_link", {"name": "ethernet", "up": True})
    wait_status(child, r"eth0: up carrier=up source=dhcp.*\r*\ndhcp=bound", 12)
    resolver(child, FIRST_DNS)
    run(child, f"{NETCTL} dhcp stop")
    wait_status(child, r"eth0: down carrier=up source=none.*\r*\ndhcp=off")
    resolver(child, None)

    run(child, f"{NETCTL} up 192.0.2.2/24 {SECOND_ROUTER}")
    before_replay = sequence(status(child), "rx_packets")
    connection.sendall(packet(peer.last_ack))
    after_replay = wait_status(child, rf"rx_packets={before_replay + 1}\b")
    assert re.search(
        r"source=static address=192\.0\.2\.2/24 gateway=192\.0\.2\.254", after_replay
    )
    run(child, 'kill "$dns_pid"; wait "$dns_pid" 2>/dev/null; true')
