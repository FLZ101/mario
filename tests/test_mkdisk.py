"""Host-only checks for disk image inputs, publication, and cleanup."""

import os
import runpy
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
