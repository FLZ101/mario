"""Guest-side checks for sshd startup, SSH logins, host keys, and SFTP/SCP."""

import os
import re
import shutil
import stat
import subprocess
import time
from pathlib import Path
from tempfile import TemporaryDirectory

SSH = "/usr/bin/ssh"
SCP = "/usr/bin/scp"
SFTP = "/usr/bin/sftp"
KEYGEN = "/usr/bin/ssh-keygen"
FORWARD_HOST = "10.0.2.100"
GUEST_ADDRESS = "10.0.2.15"
GATEWAY_ADDRESS = "10.0.2.2"
HOST_KEY = Path("/etc/ssh/ssh_host_ed25519_key")
HOST_PUBKEY = Path("/etc/ssh/ssh_host_ed25519_key.pub")
SSHD_LOG = Path("/tmp/sshd.log")
KEYGEN_LOG = Path("/tmp/ssh-keygen.log")
NETWORK_HOOK = Path("/etc/mario-network.sh")


def run_cmd(args, *, input_bytes=None, timeout=25):
    return subprocess.run(
        [str(arg) for arg in args],
        input=input_bytes,
        capture_output=True,
        timeout=timeout,
    )


def check_cmd(args, *, input_bytes=None, timeout=25):
    result = run_cmd(args, input_bytes=input_bytes, timeout=timeout)
    assert result.returncode == 0, (
        args,
        result.returncode,
        result.stdout,
        result.stderr,
    )
    return result


def check_prctl_dumpable():
    check_cmd(["/mnt/ssh_prctl"])
    check_cmd(["/usr/libexec/sftp-server", "-e"], input_bytes=b"")


def start_sshd_via_initrc():
    # NIC-less and non-GTK boots omit /etc/mario-network.sh, so sshd must not be
    # running before a network hook exists.
    assert not Path("/tmp/.mario-sshd-lock").exists()
    assert not SSHD_LOG.exists()
    assert HOST_KEY.is_file() and HOST_PUBKEY.is_file()
    assert stat.S_IMODE(HOST_KEY.stat().st_mode) == 0o600
    assert stat.S_IMODE(HOST_PUBKEY.stat().st_mode) == 0o644
    baked_priv = HOST_KEY.read_bytes()
    baked_pub = HOST_PUBKEY.read_bytes()

    # Install a network hook and run the /etc/initrc sshd block to verify it
    # reuses the pre-generated image host key without invoking ssh-keygen, starts
    # the daemon, and stays idempotent under /tmp/.mario-sshd-lock.
    NETWORK_HOOK.write_text("/sbin/netctl.exe status >/dev/null\n")
    initrc = Path("/etc/initrc").read_text()
    initrc_without_mount = "\n".join(
        line
        for line in initrc.splitlines()
        if line.strip() != "mount -t ext2 /dev/hdc5 /usr"
    )
    with TemporaryDirectory(dir="/tmp", prefix="initrc-ssh-") as scratch:
        script = Path(scratch) / "initrc"
        script.write_text(initrc_without_mount + "\n")
        subprocess.run(
            ["/bin/oksh", str(script)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=25,
            check=True,
        )
        # Running the startup sequence a second time must be a no-op because of
        # /tmp/.mario-sshd-lock.
        subprocess.run(
            ["/bin/oksh", str(script)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=25,
            check=True,
        )

    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if (
            HOST_KEY.exists()
            and HOST_PUBKEY.exists()
            and SSHD_LOG.exists()
            and "Server listening on 0.0.0.0 port 22." in SSHD_LOG.read_text()
        ):
            break
        time.sleep(0.1)
    else:
        log = SSHD_LOG.read_text() if SSHD_LOG.exists() else "<missing>"
        raise AssertionError(f"sshd did not start listening on port 22:\n{log}")

    log = SSHD_LOG.read_text()
    assert "Unsupported option" not in log, log
    assert log.count("Server listening on 0.0.0.0 port 22.") == 1, log
    assert stat.S_IMODE(HOST_KEY.stat().st_mode) == 0o600
    assert HOST_KEY.read_bytes() == baked_priv
    assert HOST_PUBKEY.read_bytes() == baked_pub
    assert not KEYGEN_LOG.exists(), KEYGEN_LOG.read_text()


def base_ssh_options(known_hosts, identity=None):
    options = [
        "-o",
        f"UserKnownHostsFile={known_hosts}",
        "-o",
        "StrictHostKeyChecking=yes",
        "-o",
        "BatchMode=yes",
        "-o",
        "IdentitiesOnly=yes",
        "-o",
        "ConnectTimeout=10",
    ]
    if identity is not None:
        options += ["-i", str(identity)]
    return options


def check_logins_and_transfers():
    ssh_dir = Path("/root/.ssh")
    shutil.rmtree(ssh_dir, ignore_errors=True)
    ssh_dir.mkdir(mode=0o700)
    authorized_keys = ssh_dir / "authorized_keys"

    with TemporaryDirectory(dir="/tmp", prefix="ssh-login-") as work:
        workdir = Path(work)
        os.chmod(workdir, 0o700)
        allowed_key = workdir / "allowed_ed25519"
        denied_key = workdir / "denied_ed25519"
        wrong_host_key = workdir / "wrong_host_ed25519"
        for key in (allowed_key, denied_key, wrong_host_key):
            check_cmd([KEYGEN, "-q", "-t", "ed25519", "-N", "", "-f", key])

        authorized_keys.write_bytes(allowed_key.with_suffix(".pub").read_bytes())
        os.chmod(authorized_keys, 0o600)

        host_pub = HOST_PUBKEY.read_text().strip()
        known_hosts = workdir / "known_hosts"
        known_hosts.write_text(f"{FORWARD_HOST} {host_pub}\n")
        bad_known_hosts = workdir / "bad_known_hosts"
        bad_known_hosts.write_text(
            f"{FORWARD_HOST} {wrong_host_key.with_suffix('.pub').read_text().strip()}\n"
        )

        # 1. Non-interactive command login over QEMU hostfwd + guestfwd.
        remote = run_cmd(
            [
                SSH,
                *base_ssh_options(known_hosts, allowed_key),
                f"root@{FORWARD_HOST}",
                "id -un && pwd && printf 'CONN=%s\\n' \"$SSH_CONNECTION\" && exit 42",
            ],
            timeout=25,
        )
        assert remote.returncode == 42, (
            remote.returncode,
            remote.stdout,
            remote.stderr,
        )
        lines = remote.stdout.decode().splitlines()
        assert lines[:2] == ["root", "/root"], lines
        conn = re.fullmatch(
            rf"CONN={re.escape(GATEWAY_ADDRESS)} \d+ {re.escape(GUEST_ADDRESS)} 22",
            lines[2],
        )
        assert conn, lines

        # 2. Stdin -> remote pipeline -> stdout round-trip over non-interactive SSH.
        stream_payload = bytes((index * 37) % 251 for index in range(8192))
        piped = check_cmd(
            [
                SSH,
                *base_ssh_options(known_hosts, allowed_key),
                f"root@{FORWARD_HOST}",
                "cat",
            ],
            input_bytes=stream_payload,
        )
        assert piped.stdout == stream_payload

        # 3. Interactive PTY login (-tt) exercises openpty + /proc/self/fd readlink
        # inside musl ttyname_r() and runs an interactive login shell.
        interactive = check_cmd(
            [
                SSH,
                "-tt",
                *base_ssh_options(known_hosts, allowed_key),
                f"root@{FORWARD_HOST}",
            ],
            input_bytes=(
                b"printf 'PTY_TTY=%s\\n' \"$(tty)\"\n"
                b"printf 'PTY_FD0=%s\\n' \"$(readlink /proc/self/fd/0)\"\n"
                b"printf 'PTY_PATH=%s\\n' \"$PATH\"\n"
                b"exit 0\n"
            ),
        )
        pty_out = interactive.stdout.decode("latin-1").replace("\r\n", "\n")
        tty_match = re.search(r"(?m)^PTY_TTY=(/dev/pts/\d+)$", pty_out)
        fd_match = re.search(r"(?m)^PTY_FD0=(/dev/pts/\d+)$", pty_out)
        assert tty_match and fd_match and tty_match[1] == fd_match[1], pty_out
        assert "PTY_PATH=/bin:/usr/bin:/bin/toybox\n" in pty_out, pty_out

        # 4. Negative authentication checks: unauthorized key, no key (password
        # disabled), and mismatched host key with StrictHostKeyChecking=yes.
        denied = run_cmd(
            [
                SSH,
                *base_ssh_options(known_hosts, denied_key),
                f"root@{FORWARD_HOST}",
                "true",
            ]
        )
        assert denied.returncode == 255, denied
        assert b"Permission denied (publickey)" in denied.stderr, denied.stderr

        no_key = run_cmd(
            [
                SSH,
                *base_ssh_options(known_hosts, workdir / "missing_key"),
                f"root@{FORWARD_HOST}",
                "true",
            ]
        )
        assert no_key.returncode == 255, no_key
        assert b"Permission denied (publickey)" in no_key.stderr, no_key.stderr

        bad_host = run_cmd(
            [
                SSH,
                *base_ssh_options(bad_known_hosts, allowed_key),
                f"root@{FORWARD_HOST}",
                "true",
            ]
        )
        assert bad_host.returncode == 255, bad_host
        assert (
            b"REMOTE HOST IDENTIFICATION HAS CHANGED" in bad_host.stderr
        ), bad_host.stderr

        # 5. SCP upload and download (default OpenSSH 10 SFTP mode).
        payload = b"Mario SSH transfer payload\n" + bytes(range(256)) * 16
        local_src = workdir / "upload.bin"
        remote_copy = workdir / "remote-copy.bin"
        local_dst = workdir / "download.bin"
        local_src.write_bytes(payload)

        check_cmd(
            [
                SCP,
                "-q",
                *base_ssh_options(known_hosts, allowed_key),
                local_src,
                f"root@{FORWARD_HOST}:{remote_copy}",
            ]
        )
        assert remote_copy.read_bytes() == payload

        check_cmd(
            [
                SCP,
                "-q",
                *base_ssh_options(known_hosts, allowed_key),
                f"root@{FORWARD_HOST}:{remote_copy}",
                local_dst,
            ]
        )
        assert local_dst.read_bytes() == payload

        # 6. Direct SFTP batch session (put, ls, get, rm).
        sftp_remote = workdir / "sftp-target.bin"
        sftp_back = workdir / "sftp-roundtrip.bin"
        batch = (
            f"put {local_src} {sftp_remote}\n"
            f"ls -l {sftp_remote}\n"
            f"get {sftp_remote} {sftp_back}\n"
            f"rm {sftp_remote}\n"
        ).encode()
        sftp_run = check_cmd(
            [
                SFTP,
                "-b",
                "-",
                *base_ssh_options(known_hosts, allowed_key),
                f"root@{FORWARD_HOST}",
            ],
            input_bytes=batch,
        )
        assert str(sftp_remote).encode() in sftp_run.stdout, sftp_run.stdout
        assert sftp_back.read_bytes() == payload
        assert not sftp_remote.exists()

    sshd_log = SSHD_LOG.read_text()
    assert "Accepted publickey for root from 10.0.2.2" in sshd_log, sshd_log


def main():
    check_prctl_dumpable()
    start_sshd_via_initrc()
    check_logins_and_transfers()
    print("NET_SSH_LOGIN_OK")


if __name__ == "__main__":
    main()
