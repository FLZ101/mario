"""Verify OpenSSH sshd logins and file transfer over QEMU port forwarding."""

import argparse
import os
import select
import shlex
import socket
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from boot import stage_fixture
from ethernet import SOURCES, finish, guest, listener, run, start, wait_status
from lit_kernel import kernel_artifacts
from lit_shell_test import require_files

HERE = Path(__file__).resolve().parent
GUEST_PYTHON = "/usr/bin/python -B /mnt/ssh_guest.py"
TIMEOUT = 15


def relay_forward(port):
    """Bridge a QEMU guestfwd stdio stream onto the hostfwd loopback listener."""
    with (
        socket.socket(fileno=os.dup(sys.stdin.fileno())) as client,
        socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT) as server,
    ):
        client.settimeout(TIMEOUT)
        server.settimeout(TIMEOUT)
        peers = {client: server, server: client}
        while peers:
            readable, _, _ = select.select(tuple(peers), (), (), TIMEOUT)
            if not readable:
                raise TimeoutError("SSH forward relay timed out")
            for source in readable:
                target = peers.get(source)
                if target is None:
                    continue
                chunk = source.recv(16384)
                if chunk:
                    target.sendall(chunk)
                else:
                    try:
                        target.shutdown(socket.SHUT_WR)
                    except OSError:
                        pass
                    peers.pop(source, None)


def check_ssh_login(smp):
    with kernel_artifacts(None), TemporaryDirectory(prefix="mario-net-ssh-") as temp:
        directory = Path(temp)
        initrd = stage_fixture(
            directory,
            {"netctl": SOURCES["netctl"], "ssh_prctl": HERE / "ssh_prctl.c"},
            files=[HERE / "ssh_guest.py"],
        )
        with listener() as reservation:
            inbound = reservation.getsockname()[1]
        relay = shlex.join(
            [sys.executable, str(Path(__file__).resolve()), "--relay", str(inbound)]
        )
        backend = (
            "user,id=wire,restrict=on"
            f",hostfwd=tcp:127.0.0.1:{inbound}-10.0.2.15:22"
            f",guestfwd=tcp:10.0.2.100:22-cmd:{relay}"
        )
        with guest(smp, directory, initrd, backend) as (child, _monitor):
            run(child, "/sbin/netctl.exe dhcp start")
            wait_status(
                child, r"source=dhcp address=10\.0\.2\.15/24.*\r*\ndhcp=bound", 12
            )
            start(child, GUEST_PYTHON)
            output = finish(child, timeout=120)
            assert "NET_SSH_LOGIN_OK" in output, output
    print(f"PASS: OpenSSH sshd login and SFTP/SCP (SMP={smp})", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int)
    parser.add_argument("--relay", type=int)
    args = parser.parse_args()
    if args.relay is not None:
        relay_forward(args.relay)
        return
    if args.smp is None:
        parser.error("--smp is required")
    require_files(require_kernel=True)
    check_ssh_login(args.smp)


if __name__ == "__main__":
    main()
