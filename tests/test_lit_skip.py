"""Host-only coverage for reasoned skips across all fixture runners."""

from contextlib import ExitStack, contextmanager, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import lit_gui_test
import lit_kernel
import lit_shell_test
import lit_tty_test


RUNNERS = (
    (lit_gui_test, lit_gui_test.GuiTest, "SERIAL: echo ready\nSERIAL-CHECK: ready"),
    (lit_shell_test, lit_shell_test.LitTest, "RUN: echo ready\nCHECK: ready"),
    (lit_tty_test, lit_tty_test.TtyLitTest, "TTY-SEND: echo ready\nTTY-CHECK: ready"),
)


class FixtureSkipTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        self.fixture = self.root / "fixture.test"

    def tearDown(self):
        self.tempdir.cleanup()

    def parse(self, factory, body):
        self.fixture.write_text(body + "\nEXIT:\n", encoding="utf-8")
        return factory(self.fixture).parse()

    def run_main(self, module, body, flags=()):
        self.fixture.write_text(body + "\nEXIT:\n", encoding="utf-8")
        output = StringIO()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(output))
            stack.enter_context(
                patch("sys.argv", ["runner", str(self.fixture), *flags])
            )
            stack.enter_context(patch.object(module, "ROOT", self.root))
            if module is lit_gui_test:
                stack.enter_context(patch.object(module, "HD_IMAGES", []))
                launch = stack.enter_context(
                    patch.object(module, "run_test", return_value=[])
                )
            elif module is lit_shell_test:
                stack.enter_context(patch.object(module, "require_files"))
                for image_name in ("HD1_IMAGE", "HD2_IMAGE", "HD3_IMAGE"):
                    stack.enter_context(
                        patch.object(module, image_name, str(self.fixture))
                    )
                launch = stack.enter_context(
                    patch.object(module, "run_lit_test", return_value=([], "", 0))
                )
            else:
                launch = stack.enter_context(
                    patch.object(module, "run_test", return_value=(0.0, [], "", ""))
                )
            result = module.main()
        self.assertIn(result, (None, 0))
        return output.getvalue(), launch

    def test_reason_is_required_and_unique(self):
        for module, factory, actions in RUNNERS:
            for directive in ("SKIP:", "SKIP:   ", "SKIP: first\nSKIP: second"):
                with self.subTest(runner=module.__name__, directive=directive):
                    with self.assertRaises((ValueError, lit_gui_test.GuiTestError)):
                        self.parse(factory, directive + "\n" + actions)

    def test_skip_preserves_parsed_assertions(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                normal = self.parse(factory, actions)
                skipped = self.parse(factory, "SKIP:  tracked regression  \n" + actions)
                self.assertEqual(skipped.skip_reason, "tracked regression")
                self.assertEqual(skipped.actions, normal.actions)

    def test_skip_is_metadata_before_kernel_rebuild(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                parsed = self.parse(
                    factory,
                    "SKIP: tracked regression\nREBUILD-KERNEL: selftest-kmalloc\n"
                    + actions,
                )
                self.assertEqual(parsed.rebuild_kernel, "selftest-kmalloc")

    def test_heredoc_contents_are_not_skip_directives(self):
        cases = (
            (lit_gui_test.GuiTest, "SERIAL: <<END\nSKIP: script data\nEND"),
            (lit_shell_test.LitTest, "RUN: cat <<'END'\nSKIP: script data\nEND"),
        )
        for factory, body in cases:
            with self.subTest(runner=factory.__name__):
                self.assertIsNone(self.parse(factory, body).skip_reason)

    def test_skipped_fixtures_do_not_launch_or_report_pass(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                output, launch = self.run_main(
                    module,
                    "SKIP: tracked regression\nREBUILD-KERNEL: selftest-kmalloc\n"
                    + actions,
                )
                launch.assert_not_called()
                self.assertIn("SKIP: fixture.test [tracked regression]", output)
                self.assertNotIn("PASS:", output)

    def test_override_runs_skipped_fixtures(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                output, launch = self.run_main(
                    module, "SKIP: tracked regression\n" + actions, ("--run-skipped",)
                )
                launch.assert_called_once()
                self.assertIn("PASS: fixture.test", output)
                self.assertNotIn("SKIP:", output)

    def test_override_does_not_bypass_smp_requirements(self):
        for module, factory, actions in RUNNERS[1:]:
            with self.subTest(runner=module.__name__):
                output, launch = self.run_main(
                    module,
                    "SKIP: tracked regression\nREQUIRES-SMP: >=32\n" + actions,
                    ("--run-skipped", "--smp", "16"),
                )
                launch.assert_not_called()
                self.assertIn("requires SMP >=32", output)

    def test_unskipped_fixtures_run_normally(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                output, launch = self.run_main(module, actions)
                launch.assert_called_once()
                self.assertIn("PASS: fixture.test", output)

    def test_gui_skip_does_not_require_disk_images(self):
        self.fixture.write_text("SKIP: tracked regression\nEXIT:\n", encoding="utf-8")
        with patch("sys.argv", ["runner", str(self.fixture)]), patch.object(
            lit_gui_test, "HD_IMAGES", [self.root / "missing.img"]
        ), patch.object(lit_gui_test, "run_test") as launch, redirect_stdout(
            StringIO()
        ):
            lit_gui_test.main()
        launch.assert_not_called()

    def test_gui_skip_does_not_hide_invalid_assertions(self):
        with self.assertRaises(lit_gui_test.GuiTestError):
            self.parse(
                lit_gui_test.GuiTest,
                "SKIP: tracked regression\nGUI-CHECK-RECT: invalid",
            )

    def test_directives_after_exit_are_not_skip_metadata(self):
        for module, factory, actions in RUNNERS:
            with self.subTest(runner=module.__name__):
                parsed = self.parse(factory, actions + "\nEXIT:\nSKIP: ignored")
                self.assertIsNone(parsed.skip_reason)

    def test_reported_elapsed_excludes_kernel_artifact_lock_and_rebuild_time(self):
        clock = [100.0]

        @contextmanager
        def fake_kernel_artifacts(*_args, **_kwargs):
            # Simulate 1175s waiting on LOCK_EX and building selftest kernel
            # before the test's QEMU session runs, and 12s restoring afterward.
            clock[0] += 1175.0
            try:
                yield
            finally:
                clock[0] += 12.0

        # GUI runner
        self.fixture.write_text(
            "REBUILD-KERNEL: selftest-fbcon-start-failure\n"
            "EXPECT-HALT: failed\nEXIT:\n",
            encoding="utf-8",
        )
        gui_out = StringIO()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(gui_out))
            stack.enter_context(patch("sys.argv", ["runner", str(self.fixture)]))
            stack.enter_context(patch.object(lit_gui_test, "ROOT", self.root))
            stack.enter_context(patch.object(lit_gui_test, "HD_IMAGES", []))
            stack.enter_context(
                patch.object(
                    lit_gui_test,
                    "rebuild_kernel_batch",
                    contextmanager(lambda *_a, **_kw: (yield)),
                )
            )
            stack.enter_context(
                patch.object(lit_gui_test, "kernel_artifacts", fake_kernel_artifacts)
            )
            stack.enter_context(
                patch.object(
                    lit_gui_test.time, "monotonic", side_effect=lambda: clock[0]
                )
            )
            stack.enter_context(
                patch.object(
                    lit_gui_test,
                    "_run_test_locked",
                    side_effect=lambda *_a, **_kw: clock.__setitem__(0, clock[0] + 3.25)
                    or [],
                )
            )
            lit_gui_test.main()
        self.assertIn("PASS: fixture.test [3.25s]", gui_out.getvalue())

        # Shell runner
        self.fixture.write_text(
            "REBUILD-KERNEL: selftest-kmalloc\nEXPECT-HALT: failed\nEXIT:\n",
            encoding="utf-8",
        )
        shell_out = StringIO()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(shell_out))
            stack.enter_context(patch("sys.argv", ["runner", str(self.fixture)]))
            stack.enter_context(patch.object(lit_shell_test, "ROOT", self.root))
            stack.enter_context(patch.object(lit_shell_test, "require_files"))
            for image_name in ("HD1_IMAGE", "HD2_IMAGE", "HD3_IMAGE"):
                stack.enter_context(
                    patch.object(lit_shell_test, image_name, str(self.fixture))
                )
            stack.enter_context(
                patch.object(
                    lit_shell_test,
                    "rebuild_kernel_batch",
                    contextmanager(lambda *_a, **_kw: (yield)),
                )
            )
            stack.enter_context(
                patch.object(lit_shell_test, "kernel_artifacts", fake_kernel_artifacts)
            )
            stack.enter_context(
                patch.object(
                    lit_shell_test.time, "monotonic", side_effect=lambda: clock[0]
                )
            )
            stack.enter_context(
                patch.object(
                    lit_shell_test,
                    "_run_lit_test_locked",
                    side_effect=lambda *_a, **_kw: clock.__setitem__(0, clock[0] + 2.50)
                    or ([], "", 0),
                )
            )
            lit_shell_test.main()
        self.assertIn("PASS: fixture.test [2.50s]", shell_out.getvalue())

        # TTY runner
        self.fixture.write_text(
            "REBUILD-KERNEL: selftest-kmalloc\nEXPECT-HALT: failed\nEXIT:\n",
            encoding="utf-8",
        )
        tty_out = StringIO()
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(tty_out))
            stack.enter_context(patch("sys.argv", ["runner", str(self.fixture)]))
            stack.enter_context(patch.object(lit_tty_test, "ROOT", self.root))
            stack.enter_context(
                patch.object(
                    lit_tty_test,
                    "rebuild_kernel_batch",
                    contextmanager(lambda *_a, **_kw: (yield)),
                )
            )
            stack.enter_context(
                patch.object(lit_tty_test, "kernel_artifacts", fake_kernel_artifacts)
            )
            stack.enter_context(
                patch.object(
                    lit_tty_test.time, "monotonic", side_effect=lambda: clock[0]
                )
            )
            fake_session = unittest.mock.MagicMock()
            fake_session.start.side_effect = lambda *_a: clock.__setitem__(
                0, clock[0] + 4.75
            )
            fake_session.capture_panel.return_value = ""
            fake_session.qemu_diagnostics.return_value = ""
            stack.enter_context(
                patch.object(
                    lit_tty_test, "TmuxSerialSession", return_value=fake_session
                )
            )
            self.assertEqual(lit_tty_test.main(), 0)
        self.assertIn("PASS: fixture.test [4.75s]", tty_out.getvalue())

    def test_rebuild_kernel_batch_restores_once_and_runs_after_ordinary_tests(self):
        makes = []
        lock_file = self.root / "artifacts.lock"
        gate_file = self.root / "artifacts.gate.lock"
        with ExitStack() as stack:
            stack.enter_context(
                patch.object(lit_kernel, "_lock_path", return_value=lock_file)
            )
            stack.enter_context(
                patch.object(lit_kernel, "_gate_lock_path", return_value=gate_file)
            )
            stack.enter_context(
                patch.object(lit_kernel, "_run_make", side_effect=makes.append)
            )
            with lit_kernel.rebuild_kernel_batch("disk-image-vbe_1024x768"):
                with lit_kernel.kernel_artifacts(
                    "selftest-fb-write-chunks", "disk-image-vbe_1024x768"
                ):
                    makes.append("guest-1")
                with lit_kernel.kernel_artifacts(
                    "selftest-fbcon-start-failure", "disk-image-vbe_1024x768"
                ):
                    makes.append("guest-2")

        self.assertEqual(
            makes,
            [
                "selftest-fb-write-chunks",
                "disk-image-vbe_1024x768",
                "guest-1",
                "selftest-fbcon-start-failure",
                "disk-image-vbe_1024x768",
                "guest-2",
                "clean-kernel",
                "kernel",
                "disk-image-vbe_1024x768",
            ],
        )

        # Verify runners execute ordinary tests first and REBUILD-KERNEL tests in
        # a single trailing batch even when alphabetically interleaved.
        rebuild_fixture = self.root / "a-rebuild.test"
        ordinary_fixture = self.root / "z-ordinary.test"
        rebuild_fixture.write_text(
            "REBUILD-KERNEL: selftest-fbcon-start-failure\nEXPECT-HALT: failed\nEXIT:\n",
            encoding="utf-8",
        )
        ordinary_fixture.write_text("SERIAL: echo ok\nEXIT:\n", encoding="utf-8")

        order = []

        @contextmanager
        def record_batch(*_a, **_kw):
            order.append("batch-enter")
            try:
                yield
            finally:
                order.append("batch-exit")

        def record_gui_run(test, *_a, **_kw):
            order.append(test.path.name)
            return []

        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(StringIO()))
            stack.enter_context(
                patch(
                    "sys.argv",
                    [
                        "runner",
                        "--jobs",
                        "2",
                        str(rebuild_fixture),
                        str(ordinary_fixture),
                    ],
                )
            )
            stack.enter_context(patch.object(lit_gui_test, "ROOT", self.root))
            stack.enter_context(patch.object(lit_gui_test, "HD_IMAGES", []))
            stack.enter_context(
                patch.object(lit_gui_test, "rebuild_kernel_batch", record_batch)
            )
            stack.enter_context(
                patch.object(lit_gui_test, "run_test", side_effect=record_gui_run)
            )
            lit_gui_test.main()

        self.assertEqual(
            order,
            ["z-ordinary.test", "batch-enter", "a-rebuild.test", "batch-exit"],
        )

    def test_rebuild_kernel_batch_restores_on_build_failure(self):
        makes = []
        lock_file = self.root / "artifacts.lock"
        gate_file = self.root / "artifacts.gate.lock"

        def fake_make(target):
            makes.append(target)
            if target == "selftest-kmalloc":
                raise lit_kernel.KernelBuildError("selftest build failed")

        with ExitStack() as stack:
            stack.enter_context(
                patch.object(lit_kernel, "_lock_path", return_value=lock_file)
            )
            stack.enter_context(
                patch.object(lit_kernel, "_gate_lock_path", return_value=gate_file)
            )
            stack.enter_context(
                patch.object(lit_kernel, "_run_make", side_effect=fake_make)
            )
            with self.assertRaises(lit_kernel.KernelBuildError):
                with lit_kernel.kernel_artifacts("selftest-kmalloc"):
                    pass

        self.assertEqual(
            makes,
            ["selftest-kmalloc", "clean-kernel", "kernel"],
        )


if __name__ == "__main__":
    unittest.main()
