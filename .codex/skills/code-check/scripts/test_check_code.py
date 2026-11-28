#!/usr/bin/env python3
"""Unit tests for check_code.py and check_kernel_locks.py."""

from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import unittest

SCRIPTS_DIR = Path(__file__).resolve().parent
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import check_code

REPO_ROOT = SCRIPTS_DIR.parents[3]


class CodeCheckTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = check_code.load_config(check_code.DEFAULT_CONFIG)

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)

    def tearDown(self):
        self.tempdir.cleanup()

    def write_file(self, relpath: str, content: str) -> Path:
        path = self.root / relpath
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def test_repository_baseline_is_clean(self):
        for subdir in ("src", "app", "tests"):
            findings = check_code.scan_target(
                REPO_ROOT / subdir, self.config, REPO_ROOT
            )
            self.assertEqual(findings, [], f"unexpected findings in {subdir}")

    def test_kernel_spinlock_user_copy_and_inversion(self):
        bad = self.write_file(
            "src/fs/bad.c",
            "void bad(void)\n"
            "{\n"
            "\tspin_lock_irqsave(&tasklist_lock, flags);\n"
            "\tmm_lock(task->mm);\n"
            "\tmemcpy_tofs(dst, src, 16);\n"
            "\tring_buffer_write(&rb, buf, 16, 1);\n"
            "\tmm_unlock(task->mm);\n"
            "\tspin_unlock_irqrestore(&tasklist_lock, flags);\n"
            "}\n",
        )
        messages = [
            msg for _, _, msg in check_code.scan_target(bad, self.config, self.root)
        ]
        self.assertTrue(
            any("tasklist_lock -> mm_lock inversion" in m for m in messages)
        )
        self.assertTrue(any("user-memory copy inside spinlock" in m for m in messages))
        self.assertTrue(any("ring_buffer user copy (u=1)" in m for m in messages))

    def test_app_batch_upload_requires_so_sndbuf(self):
        bad = self.write_file(
            "app/usr-cxx/fltk/window.cxx",
            "class Window {\n"
            "\tgui_surface_upload upload_{};\n"
            "\tvoid negotiate() {\n"
            "\t\tupload_.batch = patch_;\n"
            "\t}\n"
            "};\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("SO_SNDBUF", findings[0][2])

        good = self.write_file(
            "app/usr-cxx/fltk/good_window.cxx",
            "class Window {\n"
            "\tgui_surface_upload upload_{};\n"
            "\tbool start() {\n"
            "\t\tfd_ = connect_wm();\n"
            "\t\tint send_buffer = GUI_SURFACE_UPLOAD_BATCH_BYTES;\n"
            "\t\tbatch_ = setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0;\n"
            "\t\tupload_.batch = patch_ && batch_;\n"
            "\t\treturn true;\n"
            "\t}\n"
            "};\n",
        )
        self.assertEqual(check_code.scan_target(good, self.config, self.root), [])

    def test_app_so_sndbuf_before_connect_is_flagged(self):
        bad = self.write_file(
            "app/usr/client/main.c",
            "int open_wm(void)\n"
            "{\n"
            "\tint fd = socket(AF_UNIX, SOCK_STREAM, 0);\n"
            "\tint sz = 65536;\n"
            "\tsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));\n"
            "\tconnect(fd, (struct sockaddr *)&addr, sizeof(addr));\n"
            "\treturn fd;\n"
            "}\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("before connect()/socketpair()", findings[0][2])

    def test_wm_fork_without_detaching_stdio_is_flagged(self):
        bad = self.write_file(
            "app/gui/desktop/mario-wm/main.c",
            "static int wm_start_application(const char *path)\n"
            "{\n"
            "\tpid_t pid = fork();\n"
            "\tif (pid == 0) {\n"
            "\t\texecl(path, path, NULL);\n"
            "\t\t_exit(127);\n"
            "\t}\n"
            "\treturn 0;\n"
            "}\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("wm_detach_client_stdio()", findings[0][2])

    def test_terminal_pty_stdout_echo_is_flagged(self):
        bad = self.write_file(
            "app/usr/terminal/main.c",
            "static int terminal_drain_pty(int master, terminal_vt *vt, int *updated)\n"
            "{\n"
            "\tif (write_full(STDOUT_FILENO, buffer, count) < 0)\n"
            "\t\treturn -1;\n"
            "\treturn 0;\n"
            "}\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("STDOUT_FILENO", findings[0][2])

    def test_gui_test_launcher_race_before_shortcut_is_flagged(self):
        bad = self.write_file(
            "tests/gui/race.test",
            "GUI-VISUAL-TEST:\n"
            "GUI-SEND-KEY: f8\n"
            "GUI-CLICK: assets/desktop-default.png,10,747,35,14\n"
            "GUI-CLICK: assets/desktop-launcher.png,15,642,84,16\n"
            "SERIAL: while ! pgrep -x terminal >/dev/null; do sleep 1; done\n"
            "GUI-SEND-KEYS: meta_l,f\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("without waiting for window/surface readiness", findings[0][2])

        fixed = self.write_file(
            "tests/gui/fixed.test",
            "GUI-VISUAL-TEST:\n"
            "GUI-SEND-KEY: f8\n"
            "GUI-CLICK: assets/desktop-default.png,10,747,35,14\n"
            "GUI-CLICK: assets/desktop-launcher.png,15,642,84,16\n"
            "SERIAL: while ! pgrep -x terminal >/dev/null; do sleep 1; done\n"
            "GUI-CHECK-RECT: anywhere 64 64 17 24 39\n"
            "GUI-SEND-KEYS: meta_l,f\n",
        )
        self.assertEqual(check_code.scan_target(fixed, self.config, self.root), [])

    def test_unmounted_mnt_mutation_in_test_is_flagged(self):
        bad = self.write_file(
            "tests/gui/bad-mnt.test",
            "GUI-VISUAL-TEST:\n"
            "SERIAL: mkdir -p /mnt/work && printf 'hi' >/mnt/work/a.txt\n",
        )
        findings = check_code.scan_target(bad, self.config, self.root)
        self.assertEqual(len(findings), 1)
        self.assertIn("mutates shared /mnt", findings[0][2])


if __name__ == "__main__":
    unittest.main()
