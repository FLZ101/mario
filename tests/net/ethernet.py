"""Exercise the e1000 data path and IPv4 routes on a private Ethernet link."""

import argparse
import re
import select
import socket
import struct
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from contextlib import closing, contextmanager
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Event

import pexpect

from boot import stage_fixture
from lit_gui_test import Qmp
from lit_kernel import ROOT, kernel_artifacts
from lit_shell_test import BOOT_PROMPT, QEMU, qemu_command, require_files
from pexpect_utils import make_pexpect_raw, send_line

GUEST_MAC = bytes.fromhex("525400a30102")
PEER_MAC = bytes.fromhex("525400a30101")
BROADCAST = b"\xff" * 6
GUEST = socket.inet_aton("192.0.2.2")
PEER = socket.inet_aton("192.0.2.1")
OFF_LINK = socket.inet_aton("198.51.100.9")
PORT = 24680
SOURCES = {
    "netctl": ROOT / "app/system/netctl/main.c",
    "ethernet": Path(__file__).with_suffix(".c"),
}


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(value for (value,) in struct.iter_unpack("!H", data))
    while total > 0xFFFF:
        total = (total & 0xFFFF) + (total >> 16)
    return total ^ 0xFFFF


def ip_frame(protocol, payload, source=PEER, target=GUEST, destination=GUEST_MAC):
    header = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        len(payload) + 20,
        0,
        0,
        64,
        protocol,
        0,
        source,
        target,
    )
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    return destination + PEER_MAC + b"\x08\x00" + header + payload


def arp_frame(operation, destination, destination_ip, source=PEER):
    return (
        destination
        + PEER_MAC
        + b"\x08\x06"
        + struct.pack("!HHBBH", 1, 0x0800, 6, 4, operation)
        + PEER_MAC
        + source
        + (destination if operation == 2 else b"\0" * 6)
        + destination_ip
    )


def icmp_request(sequence):
    message = struct.pack("!BBHHH", 8, 0, 0, 231, sequence) + b"mario-ping"
    return ip_frame(1, message[:2] + struct.pack("!H", checksum(message)) + message[4:])


def packet(frame):
    return struct.pack("!I", len(frame)) + frame


def receive_exact(connection, length):
    data = bytearray()
    while len(data) < length:
        piece = connection.recv(length - len(data))
        if not piece:
            raise EOFError(f"peer closed after {len(data)} of {length} bytes")
        data.extend(piece)
    return bytes(data)


def receive_frame(connection):
    (length,) = struct.unpack("!I", receive_exact(connection, 4))
    assert 14 <= length <= 2048, length
    return receive_exact(connection, length)


def wait_frame(connection, predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        connection.settimeout(max(0.001, deadline - time.monotonic()))
        frame = receive_frame(connection)
        if predicate(frame):
            return frame
    raise TimeoutError("expected Ethernet frame not received")


def is_arp(frame, operation, target):
    return (
        frame[12:14] == b"\x08\x06"
        and frame[20:22] == struct.pack("!H", operation)
        and frame[38:42] == target
    )


def is_ip(frame, protocol, target):
    return (
        frame[12:14] == b"\x08\x00" and frame[23] == protocol and frame[30:34] == target
    )


def check_udp(frame, destination, payload):
    assert frame[:6] == PEER_MAC and frame[6:12] == GUEST_MAC, frame.hex()
    assert is_ip(frame, 17, destination) and frame[26:30] == GUEST, frame.hex()
    assert checksum(frame[14:34]) == 0, frame.hex()
    length = struct.unpack("!H", frame[38:40])[0]
    assert frame[42 : 34 + length] == payload, frame.hex()


def listener():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen()
    sock.settimeout(10)
    return sock


@contextmanager
def guest(smp, directory, initrd, backend, mac=GUEST_MAC, **qemu_options):
    command = qemu_command(QEMU, smp, **qemu_options)
    monitor = directory / "qmp"
    command[command.index("-initrd") + 1] = str(initrd)
    command += ["-qmp", f"unix:{monitor},server=on,wait=off"]
    if backend is not None:
        command += [
            "-netdev",
            backend,
            "-device",
            f"e1000,id=ethernet,netdev=wire,mac={mac.hex(':')}",
        ]
    child = pexpect.spawn(command[0], command[1:], encoding="latin-1", timeout=30)
    try:
        make_pexpect_raw(child)
        child.expect(BOOT_PROMPT)
        run(child, "mknod /root/net-rd b 1 3 && mount -t mariofs /root/net-rd /mnt")
        yield child, monitor
    except Exception:
        print(f"guest serial: {child.before[-3000:]}", flush=True)
        raise
    finally:
        child.close(force=True)


def start(child, command):
    send_line(child, f"{command}; printf '\\nETH_STATUS=%s\\n' $?")


def finish(child, expected=0, *, timeout=20):
    child.expect(r"(?m)^ETH_STATUS=(\d+)\r*\n", timeout=timeout)
    output = child.before
    assert int(child.match.group(1)) == expected, output
    child.expect(BOOT_PROMPT)
    return output


def run(child, command, expected=0):
    start(child, command)
    return finish(child, expected)


def status(child):
    return run(child, "/mnt/netctl status")


def byte_counts(child):
    output = run(child, "/mnt/ethernet stats")
    match = re.search(
        r"NET_STATS flags=7 address=192\.0\.2\.2/24 rx=(\d+) tx=(\d+)", output
    )
    assert match, output
    return tuple(map(int, match.groups()))


def wait_status(child, pattern, timeout=5):
    deadline = time.monotonic() + timeout
    while True:
        output = status(child)
        if re.search(pattern, output):
            return output
        if time.monotonic() >= deadline:
            raise AssertionError(f"expected {pattern!r} in status:\n{output}")
        time.sleep(0.05)


def ping(connection, sequence):
    connection.sendall(packet(icmp_request(sequence)))
    reply = wait_frame(
        connection,
        lambda frame: is_ip(frame, 1, PEER)
        and frame[34] == 0
        and frame[38:42] == struct.pack("!HH", 231, sequence),
    )
    ip_length = struct.unpack("!H", reply[16:18])[0]
    assert checksum(reply[34 : 14 + ip_length]) == 0, reply.hex()
    assert reply[42 : 14 + ip_length] == b"mario-ping", reply.hex()


def ring_pressure(child, connection):
    for batch in range(6):
        connection.sendall(
            b"".join(
                packet(icmp_request(1000 + batch * 128 + index)) for index in range(128)
            )
        )
        output = status(child)
        rx = int(re.search(r"rx_high_water=(\d+)", output)[1])
        capacity = int(re.search(r"rx_ring_size=(\d+)", output)[1]) - 1
        tx_busy = int(re.search(r"tx_busy=(\d+)", output)[1])
        if rx >= capacity and tx_busy:
            break
    else:
        raise AssertionError(f"ring pressure not observed: {output}")
    ping(connection, 900)


def raw_link(smp, directory, initrd):
    with listener() as wire:
        backend = f"socket,id=wire,connect=127.0.0.1:{wire.getsockname()[1]}"
        with guest(smp, directory, initrd, backend) as (child, monitor):
            connection, _address = wire.accept()
            with connection:
                wait_status(child, r"eth0: down carrier=up source=none")
                run(child, "/mnt/netctl up 192.0.2.2/24 192.0.2.1")
                run(child, "/mnt/ethernet config")
                run(child, "/mnt/netctl up 192.0.2.2/24 198.51.100.1", expected=1)
                assert "192.0.2.2/24 gateway=192.0.2.1" in status(child)
                run(child, f"/mnt/ethernet send {PORT} 198.51.100.9")
                request = wait_frame(connection, lambda frame: is_arp(frame, 1, PEER))
                assert (
                    request[28:32] == GUEST and request[:6] == BROADCAST
                ), request.hex()
                connection.sendall(packet(arp_frame(2, GUEST_MAC, GUEST)))
                outgoing = wait_frame(
                    connection, lambda frame: is_ip(frame, 17, OFF_LINK)
                )
                check_udp(outgoing, OFF_LINK, b"mario-wire")
                run(child, f"/mnt/ethernet send {PORT} 192.0.2.1")
                outgoing = wait_frame(connection, lambda frame: is_ip(frame, 17, PEER))
                check_udp(outgoing, PEER, b"mario-wire")

                connection.sendall(packet(arp_frame(1, BROADCAST, GUEST)))
                reply = wait_frame(connection, lambda frame: is_arp(frame, 2, PEER))
                assert reply[22:32] == GUEST_MAC + GUEST, reply.hex()
                ping(connection, 1)
                before_rx, before_tx = byte_counts(child)
                start(child, f"/mnt/ethernet udp {PORT}")
                child.expect(r"(?m)^ETH_READY\r*$", timeout=5)
                udp = struct.pack("!HHHH", PORT + 1, PORT, 18, 0) + b"local-peer"
                connection.sendall(packet(ip_frame(17, udp)))
                check_udp(
                    wait_frame(connection, lambda frame: is_ip(frame, 17, PEER)),
                    PEER,
                    b"local-peer",
                )
                finish(child)
                after_rx, after_tx = byte_counts(child)
                assert after_rx - before_rx >= 52 and after_tx - before_tx >= 52, (
                    (before_rx, before_tx),
                    (after_rx, after_tx),
                )
                for sequence in range(2, 26):
                    ping(connection, sequence)
                ring_pressure(child, connection)

                with closing(Qmp(str(monitor))) as qmp:
                    qmp.connect(timeout=5)
                    qmp.command("set_link", {"name": "ethernet", "up": False})
                    wait_status(
                        child, r"up carrier=down source=static address=192\.0\.2\.2"
                    )
                    run(child, f"/mnt/ethernet send {PORT} 198.51.100.9", expected=1)
                    qmp.command("set_link", {"name": "ethernet", "up": True})
                    wait_status(
                        child, r"up carrier=up source=static address=192\.0\.2\.2"
                    )
                run(child, "/mnt/netctl down")
                assert "down carrier=up source=static" in status(child)
                run(child, f"/mnt/ethernet send {PORT} 192.0.2.1", expected=1)
                run(child, "/mnt/netctl up 192.0.2.2/24")
                run(child, f"/mnt/ethernet send {PORT} 198.51.100.9", expected=1)
                connection.sendall(packet(arp_frame(1, BROADCAST, GUEST)))
                wait_frame(connection, lambda frame: is_arp(frame, 2, PEER))
                ping(connection, 26)
                output = status(child)
                assert re.search(r"interrupts=[1-9]\d*", output), output
                with closing(Qmp(str(monitor))) as qmp:
                    qmp.connect(timeout=5)
                    start(child, "/mnt/ethernet poweroff")
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        if (
                            qmp.command("query-status", timeout=2)["status"]
                            == "shutdown"
                        ):
                            break
                        time.sleep(0.05)
                    else:
                        raise AssertionError(
                            "guest did not shut down with e1000 attached"
                        )


def echo_stream(connection, send_first):
    connection.settimeout(8)
    for round_index in range(64):
        payload = bytes((round_index + index) % 251 for index in range(1024))
        if send_first:
            connection.sendall(payload)
        assert receive_exact(connection, len(payload)) == payload
        if not send_first:
            connection.sendall(payload)
    connection.shutdown(socket.SHUT_WR)
    assert connection.recv(1) == b""


def forwarded_tcp(smp, directory, initrd):
    with listener() as outbound:
        with listener() as reservation:
            inbound = reservation.getsockname()[1]
        backend = (
            f"user,id=wire,restrict=on,hostfwd=tcp:127.0.0.1:{inbound}-10.0.2.15:{PORT},"
            f"guestfwd=tcp:10.0.2.100:{PORT}-tcp:127.0.0.1:{outbound.getsockname()[1]}"
        )
        with guest(smp, directory, initrd, backend) as (child, _monitor):
            run(child, "/mnt/netctl up 10.0.2.15/24 10.0.2.2")
            start(child, f"/mnt/ethernet client {PORT} 10.0.2.100 no-peer-eof")
            connection, _address = outbound.accept()
            with connection:
                echo_stream(connection, False)
            finish(child)
            start(child, f"/mnt/ethernet server {PORT}")
            child.expect(r"(?m)^ETH_READY\r*$", timeout=5)
            with socket.create_connection(
                ("127.0.0.1", inbound), timeout=5
            ) as connection:
                echo_stream(connection, True)
            finish(child)


def tcp_segment(frame):
    if frame[12:14] != b"\x08\x00" or len(frame) < 54 or frame[23] != 6:
        return None
    tcp = 14 + (frame[14] & 15) * 4
    end = 14 + int.from_bytes(frame[16:18], "big")
    payload = frame[tcp + (frame[tcp + 12] >> 4) * 4 : end]
    return (frame[tcp + 4 : tcp + 8], payload) if payload else None


def relay(client, server, stop):
    dropped = None
    retransmitted = False
    while not stop.is_set():
        readable, _, _ = select.select((client, server), (), (), 0.05)
        for source in readable:
            frame = receive_frame(source)
            segment = tcp_segment(frame) if source is client else None
            if segment and dropped is None:
                dropped = segment
                continue
            if segment and segment == dropped:
                retransmitted = True
            target = server if source is client else client
            target.sendall(packet(frame))
    return retransmitted


def retransmission(smp, directory, initrd):
    client_dir = directory / "client"
    server_dir = directory / "server"
    client_dir.mkdir()
    server_dir.mkdir()
    with listener() as client_wire, listener() as server_wire:
        client_backend = (
            f"socket,id=wire,connect=127.0.0.1:{client_wire.getsockname()[1]}"
        )
        server_backend = (
            f"socket,id=wire,connect=127.0.0.1:{server_wire.getsockname()[1]}"
        )
        with guest(smp, client_dir, initrd, client_backend) as (client, _):
            with guest(smp, server_dir, initrd, server_backend, PEER_MAC) as (
                server,
                _,
            ):
                client_link, _ = client_wire.accept()
                server_link, _ = server_wire.accept()
                with client_link, server_link, ThreadPoolExecutor(1) as executor:
                    client_link.settimeout(4)
                    server_link.settimeout(4)
                    stop = Event()
                    forwarded = executor.submit(relay, client_link, server_link, stop)
                    try:
                        run(client, "/mnt/netctl up 192.0.2.2/24")
                        run(server, "/mnt/netctl up 192.0.2.1/24")
                        start(server, f"/mnt/ethernet server {PORT}")
                        server.expect(r"(?m)^ETH_READY\r*$", timeout=5)
                        start(client, f"/mnt/ethernet client {PORT} 192.0.2.1")
                        finish(client)
                        finish(server)
                    finally:
                        stop.set()
                    assert (
                        forwarded.result()
                    ), "did not observe retransmission of dropped TCP payload"


def check_model():
    models = subprocess.run(
        [QEMU, "-device", "help"],
        capture_output=True,
        text=True,
        check=True,
        timeout=10,
    )
    if 'name "e1000",' not in models.stdout + models.stderr:
        raise RuntimeError(f"{QEMU} lacks required e1000 PCI device")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    require_files(require_kernel=True)
    check_model()
    with kernel_artifacts(None), TemporaryDirectory(prefix="mario-net-") as temp:
        directory = Path(temp)
        initrd = stage_fixture(directory, SOURCES)
        raw_link(args.smp, directory, initrd)
        print(f"PASS: e1000 Ethernet and static IPv4 (SMP={args.smp})", flush=True)
        forwarded_tcp(args.smp, directory, initrd)
        print(f"PASS: e1000 restricted TCP forwards (SMP={args.smp})", flush=True)
        retransmission(args.smp, directory, initrd)
        print(f"PASS: e1000 TCP retransmission (SMP={args.smp})", flush=True)


if __name__ == "__main__":
    main()
