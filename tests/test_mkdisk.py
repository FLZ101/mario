"""Host-only checks for safe publication and cleanup of disk images."""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "qemu"))
import mkdisk


class DiskImageTests(unittest.TestCase):
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
            patch.object(mkdisk, "_populate_filesystem", side_effect=populate_filesystem),
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
