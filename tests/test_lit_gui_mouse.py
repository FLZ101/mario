from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

from PIL import Image

from lit_gui_test import (
    GuiTest,
    GuiTestError,
    _run_test_locked,
    qemu_command,
    send_mouse_position,
    send_mouse_wheel,
)


class AbsoluteMouseTests(unittest.TestCase):
    def parse(self, body: str) -> GuiTest:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mouse.test"
            path.write_text(body + "\nEXIT:\n", encoding="utf-8")
            return GuiTest(path).parse()

    def test_parses_absolute_pixel_positions(self):
        fixture = self.parse(
            "GUI-MOUSE-POSITION: 1023, 767\nGUI-MOUSE-BUTTON: left,down"
        )
        self.assertEqual(
            fixture.actions,
            [("mouse-position", (1023, 767)), ("mouse-button", ("left", True))],
        )

    def test_wheel_directive(self):
        fixture = self.parse("GUI-MOUSE-WHEEL: up\nGUI-MOUSE-WHEEL: down")
        self.assertEqual(fixture.actions, [("mouse-wheel", "up"), ("mouse-wheel", "down")])
        with self.assertRaisesRegex(GuiTestError, "invalid GUI-MOUSE-WHEEL"):
            self.parse("GUI-MOUSE-WHEEL: left")
        qmp = MagicMock()
        send_mouse_wheel(qmp, "down")
        qmp.command.assert_called_once_with(
            "input-send-event",
            {
                "events": [
                    {"type": "btn", "data": {"button": "wheel-down", "down": True}},
                    {"type": "btn", "data": {"button": "wheel-down", "down": False}},
                ]
            },
        )

    def test_rejects_invalid_positions(self):
        for value in ("", "1", "1,2,3", "-1,0", "0,-1", "a,1", "1.0,2"):
            with self.subTest(value=value):
                with self.assertRaisesRegex(GuiTestError, "invalid GUI-MOUSE-POSITION"):
                    self.parse(f"GUI-MOUSE-POSITION: {value}")

    def test_rejects_mixed_relative_and_absolute_motion(self):
        for directives in (
            "GUI-MOUSE-POSITION: 0,0\nGUI-MOUSE-MOVE: 1,1",
            "GUI-MOUSE-MOVE: 1,1\nGUI-MOUSE-POSITION: 0,0",
        ):
            with self.subTest(directives=directives):
                with self.assertRaisesRegex(GuiTestError, "cannot be mixed"):
                    self.parse(directives)

    def test_rejects_absolute_motion_in_halt_fixtures(self):
        with self.assertRaisesRegex(GuiTestError, "may only use SERIAL-CHECK"):
            self.parse("EXPECT-HALT: halted\nGUI-MOUSE-POSITION: 0,0")

    def test_default_launch_preserves_relative_mouse(self):
        command = qemu_command("qemu", "test.sock", 2)
        self.assertEqual(command[command.index("-machine") + 1], "vmport=off")
        self.assertEqual(command[command.index("-smp") + 1], "2")

    def test_absolute_launch_enables_vmport(self):
        command = qemu_command("qemu", "test.sock", 1, absolute_mouse=True)
        self.assertEqual(command[command.index("-machine") + 1], "vmport=on")

    def test_runner_selects_mode_from_fixture_actions(self):
        for body, mode in (
            ("GUI-MOUSE-POSITION: 1,2", "on"),
            ("GUI-MOUSE-MOVE: 1,2", "off"),
        ):
            with self.subTest(body=body):
                fixture = self.parse(body)
                with patch("lit_gui_test.pexpect.spawn", side_effect=OSError) as spawn:
                    with self.assertRaises(OSError):
                        _run_test_locked(fixture, "qemu", 0, 1)
                command = spawn.call_args.args[1]
                self.assertEqual(command[command.index("-machine") + 1], f"vmport={mode}")

    def test_positions_match_gtk_scaling(self):
        for width, height, pixel_x, pixel_y in (
            (1024, 768, 0, 0),
            (1024, 768, 713, 509),
            (1024, 768, 1023, 767),
            (1920, 1080, 1919, 1079),
        ):
            with self.subTest(size=(width, height), position=(pixel_x, pixel_y)):
                qmp = MagicMock()
                with patch(
                    "lit_gui_test.dump_screen",
                    return_value=Image.new("RGB", (width, height)),
                ):
                    send_mouse_position(qmp, pixel_x, pixel_y)
                qmp.command.assert_called_once_with(
                    "input-send-event",
                    {
                        "events": [
                            {
                                "type": "abs",
                                "data": {"axis": "x", "value": pixel_x * 32767 // width},
                            },
                            {
                                "type": "abs",
                                "data": {"axis": "y", "value": pixel_y * 32767 // height},
                            },
                        ]
                    },
                )

    def test_out_of_bounds_positions_do_not_inject_input(self):
        for position in ((-1, 0), (0, -1), (1024, 0), (0, 768)):
            with self.subTest(position=position):
                qmp = MagicMock()
                with patch(
                    "lit_gui_test.dump_screen",
                    return_value=Image.new("RGB", (1024, 768)),
                ):
                    with self.assertRaisesRegex(GuiTestError, "outside 1024x768"):
                        send_mouse_position(qmp, *position)
                qmp.command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
