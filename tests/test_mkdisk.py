"""Host-only checks for disk image inputs, publication, and cleanup."""

import os
import runpy
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "qemu"))
sys.path.insert(0, str(ROOT / "rd"))
import mkdisk


class DiskImageTests(unittest.TestCase):
    def test_gtk_boot_disk_uses_its_own_ramdisk_at_the_same_guest_path(self):
        script = Path(mkdisk.__file__).with_name("make-hd1.py")
        for variant, name, ramdisk in (
            ("0", "hd1", "rdx"),
            ("1", "hd1-gtk", "rdx-gtk"),
        ):
            with self.subTest(variant=variant), patch.dict(
                os.environ, {"MARIO_HD1_GTK": variant}
            ):
                disk = runpy.run_path(str(script))["disk"]
                self.assertEqual(disk.name, name)
                self.assertIn(
                    (Path("../rd") / ramdisk, "/rdx"), disk.partitions[-1].content
                )

    def test_gtk_usr_disk_uses_its_own_staging_tree(self):
        script = Path(mkdisk.__file__).with_name("make-hd3.py")
        for variant, name, staging in (
            ("0", "hd3", "ramdisk0_usr"),
            ("1", "hd3-gtk", "ramdisk0_usr-gtk"),
        ):
            with self.subTest(variant=variant), patch.dict(
                os.environ, {"MARIO_HD3_GTK": variant}
            ):
                disk = runpy.run_path(str(script))["disk"]
                self.assertEqual(disk.name, name)
                usr = disk.partitions[-1]
                self.assertEqual(usr.number, 5)
                self.assertEqual(usr.content, ((Path("../rd") / staging, "/"),))

    def test_gtk_overlay_adds_and_replaces_files_without_following_symlinks(self):
        copy_overlay = runpy.run_path(str(ROOT / "rd/make-rd0.py"))["copy_overlay"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "overlay"
            target = root / "staging"
            outside = root / "outside"
            for path in (source, target, outside):
                path.mkdir()
            (outside / "data").write_text("leave unchanged")
            (target / "link").symlink_to(outside, target_is_directory=True)
            (target / "file").write_text("base file")
            (target / "keep").touch()
            (target / "replace-directory").mkdir()
            (source / "file").write_text("older override")
            os.utime(source / "file", ns=(0, 0))
            (source / "link").mkdir()
            (source / "link/data").write_text("inside staging")
            (source / ".new file").write_text("new file")
            (source / "replace-directory").symlink_to("missing")

            copy_overlay(source, target)

            self.assertEqual((target / "file").read_text(), "older override")
            self.assertEqual((target / ".new file").read_text(), "new file")
            self.assertTrue((target / "keep").exists())
            self.assertFalse((target / "link").is_symlink())
            self.assertEqual((target / "link/data").read_text(), "inside staging")
            self.assertEqual((outside / "data").read_text(), "leave unchanged")
            self.assertEqual(os.readlink(target / "replace-directory"), "missing")

    def test_gtk_authorized_keys_stages_unique_host_pubkeys_with_strict_modes(self):
        module = runpy.run_path(str(ROOT / "rd/make-rd0.py"))
        stage_keys = module["stage_gtk_authorized_keys"]
        gtk_stamp = module["gtk_stamp_digest"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ssh_dir = root / "host-ssh"
            staging = root / "staging"
            ssh_dir.mkdir()
            staging.mkdir()

            # No host keys: /root/.ssh is not created.
            stage_keys(staging, ssh_dir=ssh_dir, explicit=None)
            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("MARIO_SSH_AUTHORIZED_KEYS", None)
                stage_keys(staging, ssh_dir=ssh_dir)
            self.assertFalse((staging / "root/.ssh").exists())

            ed25519 = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITestKeyOne user@host"
            rsa = "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABgQTestKeyTwo user@host"
            custom = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAITestKeyThree custom@host"
            (ssh_dir / "id_ed25519.pub").write_text(f"# comment\n{ed25519}\n\n")
            (ssh_dir / "id_ed25519").write_text("PRIVATE KEY MUST NOT BE COPIED\n")
            (ssh_dir / "authorized_keys").write_text(f"{ed25519}\n{rsa}\n")
            (ssh_dir / "work.pub").write_text(f"{custom}\n")

            with patch.dict(os.environ, {}, clear=False):
                os.environ.pop("MARIO_SSH_AUTHORIZED_KEYS", None)
                stage_keys(staging, ssh_dir=ssh_dir)

            guest_ssh = staging / "root/.ssh"
            auth_file = guest_ssh / "authorized_keys"
            self.assertEqual(stat.S_IMODE(guest_ssh.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(auth_file.stat().st_mode), 0o600)
            self.assertEqual(
                auth_file.read_text().splitlines(),
                [ed25519, rsa, custom],
            )

            # Explicit MARIO_SSH_AUTHORIZED_KEYS="" disables staging, and
            # changing the key set changes the Make stamp digest.
            explicit_file = root / "explicit.pub"
            explicit_file.write_text(f"{custom}\n")
            with patch.dict(os.environ, {"MARIO_SSH_AUTHORIZED_KEYS": ""}):
                empty_digest = gtk_stamp()
            with patch.dict(
                os.environ, {"MARIO_SSH_AUTHORIZED_KEYS": str(explicit_file)}
            ):
                key_digest = gtk_stamp()
            self.assertNotEqual(empty_digest, key_digest)

    def test_ssh_host_key_generated_only_when_missing_and_staged_with_strict_modes(
        self,
    ):
        module = runpy.run_path(str(ROOT / "rd/make-rd0.py"))
        ensure_keypair = module["ensure_ssh_host_keypair"]
        stage_host_key = module["stage_ssh_host_key"]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key_dir = root / ".ssh-host"
            staging = root / "staging"
            staging.mkdir()

            # First call generates a fresh Ed25519 host keypair with strict modes.
            priv_path, pub_path = ensure_keypair(key_dir)
            self.assertEqual(stat.S_IMODE(key_dir.stat().st_mode), 0o700)
            self.assertEqual(stat.S_IMODE(priv_path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(pub_path.stat().st_mode), 0o644)
            first_priv = priv_path.read_bytes()
            first_pub = pub_path.read_bytes()
            self.assertTrue(
                first_priv.startswith(b"-----BEGIN OPENSSH PRIVATE KEY-----\n")
            )
            self.assertTrue(first_pub.startswith(b"ssh-ed25519 "))

            # Subsequent calls keep the existing keypair untouched.
            os.chmod(priv_path, 0o644)
            priv_again, pub_again = ensure_keypair(key_dir)
            self.assertEqual((priv_again, pub_again), (priv_path, pub_path))
            self.assertEqual(priv_path.read_bytes(), first_priv)
            self.assertEqual(pub_path.read_bytes(), first_pub)
            self.assertEqual(stat.S_IMODE(priv_path.stat().st_mode), 0o600)

            # Staging copies the keypair to /etc/ssh with 0600 / 0644 modes and
            # does not overwrite a custom keypair already present in staging.
            stage_host_key(staging, key_dir=key_dir)
            guest_priv = staging / "etc/ssh/ssh_host_ed25519_key"
            guest_pub = staging / "etc/ssh/ssh_host_ed25519_key.pub"
            self.assertEqual(stat.S_IMODE(guest_priv.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(guest_pub.stat().st_mode), 0o644)
            self.assertEqual(guest_priv.read_bytes(), first_priv)
            self.assertEqual(guest_pub.read_bytes(), first_pub)

            guest_priv.write_bytes(b"custom-private-key\n")
            guest_pub.write_bytes(b"ssh-ed25519 AAAACustom\n")
            stage_host_key(staging, key_dir=key_dir)
            self.assertEqual(guest_priv.read_bytes(), b"custom-private-key\n")

            if shutil.which("ssh-keygen"):
                derived = subprocess.check_output(
                    ["ssh-keygen", "-y", "-f", str(priv_path)]
                )
                self.assertEqual(
                    derived.decode().split()[:2],
                    first_pub.decode().split()[:2],
                )

    def test_update_ca_bundle_verifies_checksum_and_updates_readme(self):
        module = runpy.run_path(str(ROOT / "util/update_ca_bundle.py"))
        latest_date = module["latest_snapshot_date"]
        parse_sha256 = module["parse_sha256_file"]
        update_bundle = module["update_bundle"]

        html = (
            '<a href="/ca/cacert-2026-09-25.pem">2026-09-25</a>'
            '<a href="/ca/cacert-2026-11-05.pem">2026-11-05</a>'
        )
        self.assertEqual(latest_date(html), "2026-11-05")

        sample_pem = (ROOT / "rd/_ramdisk0/etc/ssl/cert.pem").read_bytes()
        sample_sha = __import__("hashlib").sha256(sample_pem).hexdigest()
        self.assertEqual(
            parse_sha256(
                f"# comment\n{sample_sha}  cacert-2026-11-05.pem\n",
                expected_name="cacert-2026-11-05.pem",
            ),
            sample_sha,
        )
        with self.assertRaises(ValueError):
            parse_sha256(
                f"{sample_sha}  other.pem\n",
                expected_name="cacert-2026-11-05.pem",
            )

        responses = {
            "https://curl.se/docs/caextract.html": html.encode("utf-8"),
            "https://curl.se/ca/cacert-2026-11-05.pem.sha256": (
                f"{sample_sha}  cacert-2026-11-05.pem\n".encode("utf-8")
            ),
            "https://curl.se/ca/cacert-2026-11-05.pem": sample_pem,
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cert_out = root / "cert.pem"
            readme_out = root / "README.md"
            readme_out.write_text(
                "# CA certificates\n\n"
                "- Snapshot: [cacert-2025-01-01.pem](https://curl.se/ca/cacert-2025-01-01.pem)\n"
                f"- SHA-256: `{'0' * 64}`\n",
                encoding="utf-8",
            )
            with patch.dict(
                update_bundle.__globals__,
                {"fetch_bytes": lambda url, timeout=30: responses[url]},
            ):
                changed, snap, digest, count = update_bundle(
                    cert_path=cert_out, readme_path=readme_out, check_only=True
                )
                self.assertTrue(changed)
                self.assertFalse(cert_out.exists())

                changed, snap, digest, count = update_bundle(
                    cert_path=cert_out, readme_path=readme_out
                )
                self.assertTrue(changed)
                self.assertEqual(snap, "cacert-2026-11-05.pem")
                self.assertEqual(digest, sample_sha)
                self.assertGreater(count, 100)
                self.assertEqual(cert_out.read_bytes(), sample_pem)
                self.assertIn(
                    "- Snapshot: [cacert-2026-11-05.pem](https://curl.se/ca/cacert-2026-11-05.pem)",
                    readme_out.read_text(encoding="utf-8"),
                )
                self.assertIn(
                    f"- SHA-256: `{sample_sha}`",
                    readme_out.read_text(encoding="utf-8"),
                )

                changed_again, *_ = update_bundle(
                    cert_path=cert_out, readme_path=readme_out
                )
                self.assertFalse(changed_again)

            bad_responses = dict(
                responses,
                **{
                    "https://curl.se/ca/cacert-2026-11-05.pem": sample_pem
                    + b"\ntampered\n"
                },
            )
            with patch.dict(
                update_bundle.__globals__,
                {"fetch_bytes": lambda url, timeout=30: bad_responses[url]},
            ):
                with self.assertRaises(ValueError):
                    update_bundle(cert_path=cert_out, readme_path=readme_out)

    def test_unmount_retries_busy_filesystem(self):
        results = [subprocess.CompletedProcess([], status) for status in (1, 1, 0)]
        with patch.object(mkdisk, "_run", side_effect=results) as run_command:
            with patch.object(mkdisk.time, "sleep"):
                mkdisk._unmount(Path("mountpoint"))
        self.assertEqual(run_command.call_count, 3)

    def test_unmount_failure_is_fatal(self):
        result = subprocess.CompletedProcess(["umount", "mountpoint"], 1)
        with patch.object(mkdisk, "_run", return_value=result) as run_command:
            with patch.object(mkdisk.time, "sleep"):
                with self.assertRaises(subprocess.CalledProcessError):
                    mkdisk._unmount(Path("mountpoint"))
        self.assertEqual(run_command.call_count, 5)

    def build_image(self, root, unmount):
        disk = mkdisk.Disk("test", 1, "dos", [mkdisk.Partition(1, 1, "fat32")])
        target = root / "test.img"
        target.write_bytes(b"previous image")

        def create_image(image, size):
            self.assertEqual(target.read_bytes(), b"previous image")
            self.assertNotEqual(image, target)
            image.write_bytes(b"replacement image")

        def populate_filesystem(filesystem, content, mountpoint):
            (mountpoint / "grldr").write_bytes(b"boot loader")

        with (
            patch.object(mkdisk, "_create_empty_image", side_effect=create_image),
            patch.object(mkdisk, "_run"),
            patch.object(mkdisk, "_quiet"),
            patch.object(mkdisk, "_attach_loop", return_value="/dev/loop-test"),
            patch.object(mkdisk, "_wait_for_partition_devices"),
            patch.object(mkdisk, "_make_filesystem"),
            patch.object(
                mkdisk, "_populate_filesystem", side_effect=populate_filesystem
            ),
            patch.object(mkdisk, "_unmount", side_effect=unmount),
        ):
            mkdisk.build(disk)

    def test_failed_unmount_preserves_old_image_and_mounted_boot_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous_directory = Path.cwd()
            os.chdir(root)
            try:
                with self.assertRaises(subprocess.CalledProcessError):
                    self.build_image(root, subprocess.CalledProcessError(1, "umount"))
                self.assertEqual((root / "test.img").read_bytes(), b"previous image")
                boot_files = list((root / "tmp").glob("*/p1/grldr"))
                self.assertEqual(len(boot_files), 1)
                self.assertEqual(boot_files[0].read_bytes(), b"boot loader")
            finally:
                os.chdir(previous_directory)

    def test_success_publishes_replacement_after_unmount(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous_directory = Path.cwd()
            os.chdir(root)
            try:
                self.build_image(root, None)
                self.assertEqual((root / "test.img").read_bytes(), b"replacement image")
                self.assertEqual(list((root / "tmp").iterdir()), [])
            finally:
                os.chdir(previous_directory)


if __name__ == "__main__":
    unittest.main()
