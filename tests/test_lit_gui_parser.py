"""Host-only unit tests for GUI fixture directive parsing."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from lit_gui_test import GuiTest, GuiTestError, retain_visual_failure_diagnostics


class GuiDirectiveParserTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()
        self.path = Path(self.tempdir.name)
        Image.new("RGB", (4, 3), (1, 2, 3)).save(self.path / "golden.ppm")

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def parse(self, body: str) -> GuiTest:
        fixture = self.path / "fixture.test"
        fixture.write_text(body + "\nEXIT:\n", encoding="utf-8")
        return GuiTest(fixture).parse()

    def test_parses_visual_directives(self) -> None:
        test = self.parse(
            "\n".join(
                (
                    "GUI-VISUAL-TEST:",
                    "GUI-CHECK-SCREEN: golden.ppm",
                    "GUI-CHECK-RECT: 0 0 1 1 1 2 3",
                    "GUI-CHECK-SCREEN-RECT: golden.ppm,1,1,2,2",
                    "GUI-CHECK-DIFFERENT: golden.ppm",
                    "GUI-CHECK-MATCH: golden.ppm",
                )
            )
        )
        self.assertEqual(test.visual_test_markers, 1)
        self.assertEqual(
            [action for action, _value in test.actions],
            ["visual-test", "screen", "rect", "screen-rect", "different", "match"],
        )

    def test_rejects_missing_visual_assets(self) -> None:
        for directive in (
            "GUI-CHECK-SCREEN: missing.ppm",
            "GUI-CHECK-SCREEN-RECT: missing.ppm,0,0,1,1",
            "GUI-CHECK-DIFFERENT: missing.ppm",
            "GUI-CHECK-MATCH: missing.ppm",
        ):
            with self.subTest(directive=directive):
                with self.assertRaises(GuiTestError):
                    self.parse(directive)

    def test_rejects_invalid_visual_geometry(self) -> None:
        for geometry in ("0,0,0,1", "-1,0,1,1", "3,2,2,1", "a,0,1,1"):
            with self.subTest(geometry=geometry):
                with self.assertRaises(GuiTestError):
                    self.parse(f"GUI-CHECK-SCREEN-RECT: golden.ppm,{geometry}")

    def test_rejects_visual_marker_arguments(self) -> None:
        with self.assertRaises(GuiTestError):
            self.parse("GUI-VISUAL-TEST: desktop")

    def test_accepts_marked_and_unmarked_fixtures(self) -> None:
        marked = self.parse(
            "\n".join(
                (
                    "REBUILD-KERNEL: selftest-fb-write-generation",
                    "GUI-VISUAL-TEST:",
                    "GUI-CHECK-MATCH: golden.ppm",
                )
            )
        )
        raw = self.parse("SERIAL: echo raw-device-test")
        self.assertEqual(marked.visual_test_markers, 1)
        self.assertEqual(raw.visual_test_markers, 0)

    def test_rejects_duplicate_late_and_screenshot_free_markers(self) -> None:
        cases = (
            "\n".join(
                (
                    "GUI-VISUAL-TEST:",
                    "GUI-VISUAL-TEST:",
                    "GUI-CHECK-SCREEN: golden.ppm",
                )
            ),
            "\n".join(
                (
                    "SERIAL: echo started",
                    "GUI-VISUAL-TEST:",
                    "GUI-CHECK-SCREEN: golden.ppm",
                )
            ),
            "\n".join(("GUI-VISUAL-TEST:", "SERIAL: echo no-screen")),
        )
        for body in cases:
            with self.subTest(body=body):
                with self.assertRaises(GuiTestError):
                    self.parse(body)

    def test_retains_visual_failure_diagnostics_without_qemu(self) -> None:
        class FakeQmp:
            mouse_buttons = 3

            def command(self, execute: str, arguments=None):
                if execute == "screendump":
                    Image.new("RGB", (2, 1), (4, 5, 6)).save(arguments["filename"])
                    return {}
                if execute != "query-status":
                    raise AssertionError(f"unexpected QMP command: {execute}")
                return {"status": "running"}

        fixture = self.path / "failure.test"
        fixture.write_text("EXIT:\n", encoding="utf-8")
        diagnostics = retain_visual_failure_diagnostics(
            GuiTest(fixture), FakeQmp(), "serial diagnostic tail", 0, self.path / "out"
        )
        self.assertTrue((diagnostics / "actual.ppm").is_file())
        self.assertEqual(
            (diagnostics / "serial-tail.txt").read_text(encoding="latin-1"),
            "serial diagnostic tail",
        )
        state = json.loads((diagnostics / "qmp.json").read_text(encoding="utf-8"))
        self.assertEqual(state["query-status"], {"status": "running"})
        self.assertEqual(state["mouse-buttons"], 3)


if __name__ == "__main__":
    unittest.main()
