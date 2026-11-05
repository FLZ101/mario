"""Host-only unit tests for GUI fixture directive parsing."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from PIL import Image

from lit_gui_test import GuiTest, GuiTestError


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


if __name__ == "__main__":
    unittest.main()
