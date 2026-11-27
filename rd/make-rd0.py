"""Build the standard or GTK root ramdisk."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import pwd
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import copy_files


RD_DIR = Path(__file__).resolve().parent
SSH_HOST_KEY_DIR = RD_DIR / ".ssh-host"
SSH_HOST_KEY_NAME = "ssh_host_ed25519_key"
HOST_PUBKEY_ENV = "MARIO_SSH_AUTHORIZED_KEYS"
DEFAULT_HOST_PUBKEYS = (
    "id_ed25519.pub",
    "id_ecdsa.pub",
    "id_rsa.pub",
    "id_ed25519_sk.pub",
    "id_ecdsa_sk.pub",
    "authorized_keys",
)

# RFC 8032 Ed25519 domain parameters for pure-Python OpenSSH host-key generation
# (so CI runners without openssh-client on the host can still build images).
_ED25519_P = 2**255 - 19
_ED25519_D = -121665 * pow(121666, _ED25519_P - 2, _ED25519_P) % _ED25519_P
_ED25519_I = pow(2, (_ED25519_P - 1) // 4, _ED25519_P)
_ED25519_BY = 4 * pow(5, _ED25519_P - 2, _ED25519_P) % _ED25519_P


def _ed25519_xrecover(y: int) -> int:
    xx = (y * y - 1) * pow(_ED25519_D * y * y + 1, _ED25519_P - 2, _ED25519_P)
    xx %= _ED25519_P
    x = pow(xx, (_ED25519_P + 3) // 8, _ED25519_P)
    if (x * x - xx) % _ED25519_P != 0:
        x = (x * _ED25519_I) % _ED25519_P
    if x % 2 != 0:
        x = _ED25519_P - x
    return x


_ED25519_BX = _ed25519_xrecover(_ED25519_BY)
_ED25519_BASE = (
    _ED25519_BX % _ED25519_P,
    _ED25519_BY % _ED25519_P,
    1,
    (_ED25519_BX * _ED25519_BY) % _ED25519_P,
)


def _ed25519_add(
    p1: tuple[int, int, int, int], p2: tuple[int, int, int, int]
) -> tuple[int, int, int, int]:
    x1, y1, z1, t1 = p1
    x2, y2, z2, t2 = p2
    a = (y1 - x1) * (y2 - x2) % _ED25519_P
    b = (y1 + x1) * (y2 + x2) % _ED25519_P
    c = t1 * 2 * _ED25519_D * t2 % _ED25519_P
    d = z1 * 2 * z2 % _ED25519_P
    e = (b - a) % _ED25519_P
    f = (d - c) % _ED25519_P
    g = (d + c) % _ED25519_P
    h = (b + a) % _ED25519_P
    return (
        e * f % _ED25519_P,
        g * h % _ED25519_P,
        f * g % _ED25519_P,
        e * h % _ED25519_P,
    )


def _ed25519_public_from_seed(seed: bytes) -> bytes:
    clamped = bytearray(hashlib.sha512(seed).digest()[:32])
    clamped[0] &= 248
    clamped[31] &= 127
    clamped[31] |= 64
    scalar = int.from_bytes(clamped, "little")

    result = (0, 1, 1, 0)
    current = _ED25519_BASE
    while scalar > 0:
        if scalar & 1:
            result = _ed25519_add(result, current)
        current = _ed25519_add(current, current)
        scalar >>= 1

    x, y, z, _ = result
    z_inv = pow(z, _ED25519_P - 2, _ED25519_P)
    x = (x * z_inv) % _ED25519_P
    y = (y * z_inv) % _ED25519_P
    encoded = bytearray(y.to_bytes(32, "little"))
    encoded[31] |= (x & 1) << 7
    return bytes(encoded)


def _ssh_string(payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + payload


def generate_openssh_ed25519_keypair(
    comment: str = "root@mario",
) -> tuple[bytes, bytes]:
    """Generate an unencrypted OpenSSH Ed25519 (private_pem, public_line) pair."""
    seed = os.urandom(32)
    pub_raw = _ed25519_public_from_seed(seed)
    keytype = b"ssh-ed25519"
    pub_blob = _ssh_string(keytype) + _ssh_string(pub_raw)
    checkint = int.from_bytes(os.urandom(4), "big")
    priv_inner = (
        struct.pack(">II", checkint, checkint)
        + _ssh_string(keytype)
        + _ssh_string(pub_raw)
        + _ssh_string(seed + pub_raw)
        + _ssh_string(comment.encode("utf-8"))
    )
    pad_len = (8 - (len(priv_inner) % 8)) % 8
    priv_inner += bytes(range(1, pad_len + 1))
    outer = (
        b"openssh-key-v1\x00"
        + _ssh_string(b"none")
        + _ssh_string(b"none")
        + _ssh_string(b"")
        + struct.pack(">I", 1)
        + _ssh_string(pub_blob)
        + _ssh_string(priv_inner)
    )
    encoded = base64.b64encode(outer).decode("ascii")
    lines = ["-----BEGIN OPENSSH PRIVATE KEY-----"]
    lines.extend(encoded[index : index + 70] for index in range(0, len(encoded), 70))
    lines.append("-----END OPENSSH PRIVATE KEY-----\n")
    priv_bytes = "\n".join(lines).encode("ascii")
    pub_bytes = (
        f"ssh-ed25519 {base64.b64encode(pub_blob).decode('ascii')} {comment}\n"
    ).encode("utf-8")
    return priv_bytes, pub_bytes


def _atomic_write(target: Path, data: bytes, mode: int) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
        os.chmod(tmp_path, mode)
        tmp_path.replace(target)
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise


def ensure_ssh_host_keypair(key_dir: Path = SSH_HOST_KEY_DIR) -> tuple[Path, Path]:
    """Return the persistent image SSH host keypair, generating it once if missing."""
    priv_path = key_dir / SSH_HOST_KEY_NAME
    pub_path = key_dir / f"{SSH_HOST_KEY_NAME}.pub"
    if priv_path.is_file() and pub_path.is_file():
        os.chmod(key_dir, 0o700)
        os.chmod(priv_path, 0o600)
        os.chmod(pub_path, 0o644)
        return priv_path, pub_path

    key_dir.mkdir(parents=True, exist_ok=True)
    os.chmod(key_dir, 0o700)
    priv_bytes, pub_bytes = generate_openssh_ed25519_keypair()
    _atomic_write(priv_path, priv_bytes, 0o600)
    _atomic_write(pub_path, pub_bytes, 0o644)
    return priv_path, pub_path


def stage_ssh_host_key(staging: Path, key_dir: Path = SSH_HOST_KEY_DIR) -> None:
    """Install the persistent Ed25519 host keypair into /etc/ssh unless provided."""
    ssh_etc = staging / "etc" / "ssh"
    ssh_etc.mkdir(parents=True, exist_ok=True)
    os.chmod(ssh_etc, 0o755)
    guest_priv = ssh_etc / SSH_HOST_KEY_NAME
    guest_pub = ssh_etc / f"{SSH_HOST_KEY_NAME}.pub"
    if guest_priv.is_file() and guest_pub.is_file():
        os.chmod(guest_priv, 0o600)
        os.chmod(guest_pub, 0o644)
        return
    priv_src, pub_src = ensure_ssh_host_keypair(key_dir)
    shutil.copy2(priv_src, guest_priv)
    os.chmod(guest_priv, 0o600)
    shutil.copy2(pub_src, guest_pub)
    os.chmod(guest_pub, 0o644)


def write_rd0(staging: Path, image: str) -> None:
    subprocess.run(
        [
            str(RD_DIR / "rd-make.exe"),
            f"--dir={staging}",
            f"--rd={image}",
            "--device-list=device_list",
            "--nr-block=5120",
        ],
        cwd=RD_DIR,
        check=True,
    )


def build_rd0() -> None:
    staging = RD_DIR / "ramdisk0"
    shutil.rmtree(staging, ignore_errors=True)

    copy_files.copy_rootfs_files(staging)
    stage_ssh_host_key(staging)

    usr = RD_DIR / "ramdisk0_usr"
    shutil.rmtree(usr, ignore_errors=True)
    os.rename(staging / "usr", usr)

    # init mounts the separately built ramdisk0_usr image at /usr, so the
    # root ramdisk still needs an empty mountpoint for that directory.
    (staging / "usr").mkdir(parents=True, exist_ok=True)
    write_rd0(staging, "rd0")


def copy_overlay(source: Path, target: Path) -> None:
    """Merge directories and replace other entries without following symlinks."""
    if not source.is_symlink() and source.is_dir():
        if target.is_symlink() or (target.exists() and not target.is_dir()):
            target.unlink()
        target.mkdir(exist_ok=True)
        for child in source.iterdir():
            copy_overlay(child, target / child.name)
        shutil.copystat(source, target)
    else:
        if not target.is_symlink() and target.is_dir():
            shutil.rmtree(target)
        else:
            target.unlink(missing_ok=True)
        shutil.copy2(source, target, follow_symlinks=False)


def host_ssh_dir() -> Path:
    """Locate the invoking user's ~/.ssh directory, even when run via sudo."""
    sudo_user = os.environ.get("SUDO_USER")
    if sudo_user and sudo_user != "root":
        try:
            return Path(pwd.getpwnam(sudo_user).pw_dir) / ".ssh"
        except KeyError:
            pass
    return Path.home() / ".ssh"


def collect_host_authorized_keys(
    ssh_dir: Path | None = None,
    explicit: str | None = None,
) -> list[str]:
    """Collect unique public-key lines from MARIO_SSH_AUTHORIZED_KEYS or ~/.ssh."""
    if explicit is None:
        explicit = os.environ.get(HOST_PUBKEY_ENV)
    if explicit == "":
        return []
    if explicit is not None:
        candidates = [
            Path(item).expanduser() for item in explicit.split(os.pathsep) if item
        ]
        for candidate in candidates:
            if not candidate.is_file():
                raise FileNotFoundError(
                    f"{HOST_PUBKEY_ENV} entry does not exist: {candidate}"
                )
    else:
        base = host_ssh_dir() if ssh_dir is None else ssh_dir
        candidates = [
            base / name for name in DEFAULT_HOST_PUBKEYS if (base / name).is_file()
        ]
        if base.is_dir():
            known = set(candidates)
            for extra in sorted(base.glob("*.pub")):
                if extra.is_file() and extra not in known:
                    candidates.append(extra)

    keys: list[str] = []
    seen: set[str] = set()
    for candidate in candidates:
        for raw_line in candidate.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line not in seen:
                seen.add(line)
                keys.append(line)
    return keys


def stage_gtk_authorized_keys(
    staging: Path,
    ssh_dir: Path | None = None,
    explicit: str | None = None,
) -> None:
    """Install host SSH public keys into /root/.ssh/authorized_keys for GTK boots."""
    keys = collect_host_authorized_keys(ssh_dir=ssh_dir, explicit=explicit)
    if not keys:
        return
    guest_ssh = staging / "root" / ".ssh"
    guest_ssh.mkdir(parents=True, exist_ok=True)
    os.chmod(guest_ssh, 0o700)
    authorized_keys = guest_ssh / "authorized_keys"
    existing = []
    if authorized_keys.is_file():
        existing = [
            line.strip()
            for line in authorized_keys.read_text(encoding="utf-8").splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
    merged = list(dict.fromkeys([*existing, *keys]))
    authorized_keys.write_text("\n".join(merged) + "\n", encoding="utf-8")
    os.chmod(authorized_keys, 0o600)


def gtk_stamp_digest() -> str:
    """Hash the GTK overlay metadata and resolved host SSH public keys for Make."""
    digest = hashlib.sha256()
    overlay = RD_DIR / "_ramdisk0-gtk"
    if overlay.is_dir():
        entries = [overlay, *sorted(overlay.rglob("*"))]
        for entry in entries:
            stat_result = entry.lstat()
            rel = entry.relative_to(RD_DIR).as_posix()
            digest.update(
                f"overlay:{rel}:{stat_result.st_mode}:{stat_result.st_size}:{stat_result.st_mtime_ns}\0".encode()
            )
    for key in collect_host_authorized_keys():
        digest.update(f"ssh:{key}\0".encode())
    return digest.hexdigest()


def build_gtk_rd0() -> None:
    overlay = RD_DIR / "_ramdisk0-gtk"
    with tempfile.TemporaryDirectory(prefix="ramdisk0-gtk-", dir=RD_DIR) as directory:
        work = Path(directory)
        staging = work / "root"
        shutil.copytree(RD_DIR / "ramdisk0", staging, symlinks=True)
        shutil.copytree(
            RD_DIR / "ramdisk0_usr", staging / "usr", dirs_exist_ok=True, symlinks=True
        )
        if overlay.exists():
            for entry in overlay.iterdir():
                copy_overlay(entry, staging / entry.name)
        stage_gtk_authorized_keys(staging)

        usr = staging / "usr"
        if usr.is_symlink() or not usr.is_dir():
            raise ValueError("GTK overlay /usr must be a directory")
        staged_usr = usr.rename(work / "usr")
        usr.mkdir()
        image = work / "rd0-gtk"
        write_rd0(staging, str(image))

        gtk_usr = RD_DIR / "ramdisk0_usr-gtk"
        if gtk_usr.exists():
            shutil.rmtree(gtk_usr)
        staged_usr.rename(gtk_usr)
        os.utime(gtk_usr, None)
        image.replace(RD_DIR / image.name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--deps", action="store_true", help="print rd0 prerequisites for Make"
    )
    mode.add_argument(
        "--gtk", action="store_true", help="build rd0-gtk from staged rd0 files"
    )
    mode.add_argument(
        "--gtk-stamp",
        action="store_true",
        help="print the input digest for rd0-gtk (overlay + host SSH public keys)",
    )
    args = parser.parse_args(argv)

    if args.deps:
        copy_files.copy_rootfs_files(
            None,
            print_deps=True,
        )
        for key_path in ensure_ssh_host_keypair():
            print(key_path)
        return 0

    if args.gtk_stamp:
        print(gtk_stamp_digest())
        return 0

    if args.gtk:
        build_gtk_rd0()
    else:
        build_rd0()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
