"""Smoke-test the installed binaries using local DNS, HTTP, TLS, and UDP peers."""

import re
import socket
import stat
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory

from http_fixture import BODY
from python_guest import HTTP_IP, HTTP_NAME, LOCAL, local_dns
from tls_fixture import HTTPS_NAME, WRONG_NAME

MISSING_NAME = "missing.mario.test"
TOYBOX = "/bin/toybox/toybox"
IFCONFIG = "/usr/bin/ifconfig"
ROUTE = "/usr/bin/route"
NETSTAT = "/usr/bin/netstat"
NETCTL = "/sbin/netctl.exe"
LOOPBACK_ROUTE = b"127.0.0.0 0.0.0.0 255.0.0.0 U - - - lo".split()


def check(command, status=0, *, data=None):
    result = subprocess.run(command, input=data, capture_output=True, timeout=12)
    assert result.returncode == status, (command, result.returncode, result.stderr)
    return result.stdout


def check_downloads(http, tls, refused_port):
    for command, ca_flag, resolve_error, connect_error, cert_error in (
        (["/usr/bin/curl", "-q", "-fsS", "--max-time", "8"], "--cacert", 6, 7, 60),
        (
            [
                "/usr/bin/wget",
                "--no-config",
                "--no-proxy",
                "--no-hsts",
                "--tries=1",
                "--timeout=8",
                "-qO",
                "-",
            ],
            "--ca-certificate",
            4,
            4,
            5,
        ),
    ):
        for host in (HTTP_IP, HTTP_NAME):
            assert check(command + [f"http://{host}:{http}/payload"]) == BODY
        secure = f"https://{HTTPS_NAME}:{tls}/payload"
        assert check(command + [ca_flag, "/mnt/ca.pem", secure]) == BODY
        check(command + [secure], cert_error)
        check(
            command + [ca_flag, "/mnt/ca.pem", f"https://{WRONG_NAME}:{tls}/payload"],
            cert_error,
        )
        check(command + [f"http://{MISSING_NAME}:{http}/payload"], resolve_error)
        check(command + [f"http://{LOCAL}:{refused_port}/payload"], connect_error)


def check_toybox(http):
    assert HTTP_IP.encode() in check([TOYBOX, "host", HTTP_NAME, LOCAL])
    assert (
        check([TOYBOX, "wget", "-O", "-", f"http://{HTTP_NAME}:{http}/payload"]) == BODY
    )
    request = (
        b"GET /payload HTTP/1.1\r\nHost: http.mario.test\r\nConnection: close\r\n\r\n"
    )
    response = check(
        [TOYBOX, "nc", "-4", "-w", "6", "-W", "6", HTTP_NAME, str(http)], data=request
    )
    headers, payload = response.split(b"\r\n\r\n", 1)
    assert headers.startswith(b"HTTP/1.1 200 ") and payload == BODY, response[:160]


def check_icmp_tools():
    ping = check([TOYBOX, "ping", "-c", "1", "-W", "2", "127.0.0.1"])
    assert b"1 packets transmitted, 1 received, 0% packet loss" in ping, ping
    trace = subprocess.run(
        [TOYBOX, "traceroute", "-n", "-m", "2", "-w", "2", "127.0.0.1"],
        capture_output=True,
        timeout=30,
    )
    assert trace.returncode == 0, (trace.returncode, trace.stderr)
    trace = trace.stdout
    assert trace.startswith(b"traceroute to 127.0.0.1"), trace
    assert b"\n 1 127.0.0.1" in trace, trace


def check_tftp():
    def serve(peer):
        request, client = peer.recvfrom(1024)
        assert request == b"\0\1payload\0octet\0", request
        for block, payload in enumerate((BODY[:512], BODY[512:], b""), start=1):
            peer.sendto(struct.pack("!HH", 3, block) + payload, client)
            assert peer.recvfrom(1024) == (struct.pack("!HH", 4, block), client)

    with socket.socket(type=socket.SOCK_DGRAM) as peer, ThreadPoolExecutor(1) as pool:
        peer.bind((LOCAL, 0))
        peer.settimeout(5)
        server = pool.submit(serve, peer)
        assert (
            check(
                [
                    "/usr/bin/sbase/tftp",
                    "-h",
                    LOCAL,
                    "-p",
                    str(peer.getsockname()[1]),
                    "payload",
                ]
            )
            == BODY
        )
        server.result(timeout=5)


def check_ssh_client():
    version = subprocess.run(["/usr/bin/ssh", "-V"], capture_output=True, timeout=12)
    assert version.returncode == 0 and b"OpenSSH_" in version.stderr, version
    settings = check(["/usr/bin/ssh", "-G", "-T", "localhost"]).splitlines()
    assert {b"addressfamily inet", b"ipqos none none", b"tcpkeepalive false"} <= set(
        settings
    ), settings
    keygen = "/usr/bin/ssh-keygen"
    with TemporaryDirectory(dir="/root") as directory:
        key = Path(directory) / "id_ed25519"
        check([keygen, "-q", "-t", "ed25519", "-N", "", "-f", key])
        assert stat.S_IMODE(key.stat().st_mode) == 0o600
        assert check([keygen, "-y", "-f", key]).startswith(b"ssh-ed25519 ")


def check_routes(*ethernet, compare_netstat=False):
    routes = check([ROUTE, "-n"])
    if compare_netstat:
        assert routes == check([NETSTAT, "-rn"]) == check([NETSTAT, "-nr"]), routes
    assert routes.startswith(b"Kernel IP routing table\n"), routes
    assert [line.split() for line in routes.splitlines()[2:]] == [
        *(row.split() for row in ethernet),
        LOOPBACK_ROUTE,
    ], routes


def check_views():
    interface = check([IFCONFIG, "eth0"])
    assert b"eth0: flags=67<UP,BROADCAST,RUNNING>  mtu 1500" in interface, interface
    assert b"inet 10.0.2.15  netmask 255.255.255.0  broadcast 10.0.2.255" in interface
    assert b"ether 52:54:00:a3:01:02" in interface, interface
    for direction in (b"RX", b"TX"):
        counters = re.search(
            rb"%s packets ([1-9]\d*)  bytes ([1-9]\d*)" % direction, interface
        )
        assert counters and int(counters[2]) >= int(counters[1]), interface
    table = check([NETSTAT, "-i"])
    ethernet = next(
        line.split() for line in table.splitlines() if line.startswith(b"eth0 ")
    )
    assert ethernet[1] == b"1500" and ethernet[-1] == b"BRU", table
    assert int(ethernet[2]) > 0 and int(ethernet[6]) > 0, table
    assert ethernet[3] == ethernet[8] == ethernet[9] == b"-", table
    check_routes(b"10.0.2.0 0.0.0.0 255.255.255.0 U - - - eth0", compare_netstat=True)

    for command in (
        [IFCONFIG, "eth0", "down"],
        [NETSTAT],
        [NETSTAT, "-an"],
        [ROUTE, "add", "default"],
    ):
        result = subprocess.run(command, capture_output=True, timeout=12)
        assert result.returncode == 2 and b"unsupported" in result.stderr, result
    assert b"<UP," in check([IFCONFIG, "eth0"])

    check([NETCTL, "down"])
    assert b"eth0:" not in check([IFCONFIG])
    assert b"eth0:" in check([IFCONFIG, "-a"])
    check_routes(compare_netstat=True)
    check([NETCTL, "up", "192.0.2.34/27", "192.0.2.33"])
    assert b"inet 192.0.2.34  netmask 255.255.255.224  broadcast 192.0.2.63" in check(
        [IFCONFIG]
    )
    check_routes(
        b"0.0.0.0 192.0.2.33 0.0.0.0 UG - - - eth0",
        b"192.0.2.32 0.0.0.0 255.255.255.224 U - - - eth0",
    )


def check_no_nic():
    loopback = check([IFCONFIG, "lo"])
    assert loopback == check([IFCONFIG]) == check([IFCONFIG, "-a"]), loopback
    assert b"inet 127.0.0.1  netmask 255.0.0.0" in loopback, loopback
    result = subprocess.run([IFCONFIG, "eth0"], capture_output=True, timeout=12)
    assert result.returncode == 1 and b"device not found" in result.stderr, result
    table = check([NETSTAT, "-i"])
    assert table.splitlines()[2].split() == [b"lo", *([b"-"] * 9), b"LRU"], table
    assert b"eth0" not in table, table
    check_routes()
    print("NET_TOOLS_NO_NIC_OK")


def main(http, tls):
    with socket.socket() as refused:
        refused.bind((LOCAL, 0))
        records = {name: HTTP_IP for name in (HTTP_NAME, HTTPS_NAME, WRONG_NAME)}
        with local_dns(records) as names:
            check_downloads(http, tls, refused.getsockname()[1])
            check_toybox(http)
            check_icmp_tools()
            assert {*records, MISSING_NAME} <= set(names), names
    check_tftp()
    check_ssh_client()
    print("NET_TOOLS_OK")


if __name__ == "__main__":
    if sys.argv[1:] == ["--no-nic"]:
        check_no_nic()
    elif sys.argv[1:] == ["--views"]:
        check_views()
        print("NET_TOOLS_VIEWS_OK")
    else:
        main(*map(int, sys.argv[1:]))
