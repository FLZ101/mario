"""Host-only coverage for reasoned skips across all fixture runners."""

from contextlib import ExitStack, redirect_stdout
from io import StringIO
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import lit_gui_test
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
        self.fixture.write_text(
            "SKIP: tracked regression\nEXIT:\n", encoding="utf-8"
        )
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


if __name__ == "__main__":
    unittest.main()
