"""Host-only unit tests for GUI fixture directives and execution."""

from __future__ import annotations

import json
import tempfile
import unittest
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import call, patch

from PIL import Image

from lit_gui_test import (
    DEFAULT_TIMEOUT,
    FRAME_MISSING,
    FRAME_PRESENT_BEGIN,
    FRAME_PRESENT_END,
    GuiTest,
    GuiTestError,
    VISUAL_CHECK_TIMEOUT,
    _run_test_locked,
    parse_frame_probe,
    retain_visual_failure_diagnostics,
    wait_for_frame_advance,
)


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

    def test_parses_and_validates_serial_wait_file(self) -> None:
        test = self.parse("SERIAL-WAIT-FILE: /tmp/gui.ready")
        self.assertIn(("serial-wait-file", "/tmp/gui.ready"), test.actions)
        for value in ("", "tmp/gui.ready"):
            with self.subTest(value=value):
                with self.assertRaises(GuiTestError):
                    self.parse(f"SERIAL-WAIT-FILE: {value}")

    def test_parses_init_window_manager_baseline(self) -> None:
        test = self.parse("INIT-WM: keep\nGUI-CHECK-RECT: 0 0 1 1 1 2 3")
        self.assertTrue(test.keep_init_wm)
        for body in (
            "INIT-WM: restart",
            "SERIAL: true\nINIT-WM: keep",
            "INIT-WM: keep\nINIT-WM: keep",
            "INIT-WM: keep\nREBUILD-KERNEL: selftest-kmalloc",
        ):
            with self.subTest(body=body):
                with self.assertRaises(GuiTestError):
                    self.parse(body)

    def test_parses_and_validates_wait_frame(self) -> None:
        test = self.parse("GUI-WAIT-FRAME:")
        self.assertIn(("wait-frame", None), test.actions)
        with self.assertRaises(GuiTestError):
            self.parse("GUI-WAIT-FRAME: /tmp/mario-wm.frame")

    def test_parses_and_validates_key_chords(self) -> None:
        test = self.parse("GUI-SEND-KEYS: alt, tab")
        self.assertIn(("send-keys", ("alt", "tab")), test.actions)
        for value in ("", "alt,", "alt, tab-1"):
            with self.subTest(value=value):
                with self.assertRaises(GuiTestError):
                    self.parse(f"GUI-SEND-KEYS: {value}")

    def test_rejects_wait_frame_in_a_halt_fixture(self) -> None:
        with self.assertRaises(GuiTestError):
            self.parse("EXPECT-HALT: expected\nGUI-WAIT-FRAME:")

    def test_waits_for_an_advanced_frame_after_removal(self) -> None:
        values = iter((41, None, 42))
        clock = [0.0]

        def pause(seconds: float) -> None:
            clock[0] += seconds

        self.assertEqual(
            wait_for_frame_advance(
                lambda: next(values), 41, 1.0, now=lambda: clock[0], pause=pause
            ),
            42,
        )

    def test_rejects_malformed_frame_content(self) -> None:
        malformed = f"{FRAME_PRESENT_BEGIN}not-a-number\n{FRAME_PRESENT_END}"
        with self.assertRaises(GuiTestError):
            parse_frame_probe(malformed)

    def test_parses_missing_frame_content(self) -> None:
        self.assertIsNone(parse_frame_probe(FRAME_MISSING))

    def test_ignores_markers_echoed_in_a_present_frame_probe(self) -> None:
        command_echo = f"{FRAME_PRESENT_BEGIN} {FRAME_PRESENT_END} {FRAME_MISSING}"
        output = f"{command_echo}\n{FRAME_PRESENT_BEGIN}42\n{FRAME_PRESENT_END}"
        self.assertEqual(parse_frame_probe(output), 42)

    def test_reports_frame_wait_timeout(self) -> None:
        clock = [0.0]

        def pause(seconds: float) -> None:
            clock[0] += seconds

        with self.assertRaisesRegex(GuiTestError, r"greater than 9.*last value: 9"):
            wait_for_frame_advance(lambda: 9, 9, 0.1, now=lambda: clock[0], pause=pause)

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

    def test_caps_all_visual_check_timeouts_without_shortening_serial_waits(
        self,
    ) -> None:
        cases = (
            (None, VISUAL_CHECK_TIMEOUT),
            (120.0, VISUAL_CHECK_TIMEOUT),
            (1.0, 1.0),
        )
        for fixture_timeout, expected_timeout in cases:
            for matched in (True, False):
                with self.subTest(timeout=fixture_timeout, matched=matched):
                    directives = ["INIT-WM: keep", "GUI-VISUAL-TEST:"]
                    if fixture_timeout is not None:
                        directives.append(f"TIMEOUT: {fixture_timeout}")
                    directives.extend(
                        (
                            "SERIAL: true",
                            "SERIAL-WAIT-FILE: /tmp/gui.ready",
                            "GUI-CHECK-SCREEN: golden.ppm",
                            "GUI-CHECK-RECT: 0 0 1 1 1 2 3",
                            "GUI-CHECK-SCREEN-RECT: golden.ppm,1,1,2,2",
                            "GUI-CHECK-DIFFERENT: golden.ppm",
                            "GUI-CHECK-MATCH: golden.ppm",
                        )
                    )
                    fixture = self.parse("\n".join(directives))
                    with ExitStack() as stack:
                        mocked = {}
                        for name in (
                            "pexpect.spawn",
                            "make_pexpect_raw",
                            "send_line",
                            "expect_prompt",
                            "wait_for_guest_file",
                            "Qmp",
                            "time.sleep",
                            "os.unlink",
                            "retain_visual_failure_diagnostics",
                        ):
                            mocked[name] = stack.enter_context(
                                patch(f"lit_gui_test.{name}")
                            )
                        checks = {}
                        for name in (
                            "wait_for_screen",
                            "wait_for_solid_rect",
                            "wait_for_screen_rect",
                            "wait_for_screen_match",
                        ):
                            checks[name] = stack.enter_context(
                                patch(f"lit_gui_test.{name}", return_value=matched)
                            )
                        child = mocked["pexpect.spawn"].return_value
                        child.before = ""
                        mocked["wait_for_guest_file"].return_value = "ready"
                        failures = _run_test_locked(fixture, "qemu", 0, 1)
                    qmp = mocked["Qmp"].return_value
                    for helper, action_index in (
                        ("wait_for_screen", -5),
                        ("wait_for_solid_rect", -4),
                        ("wait_for_screen_rect", -3),
                    ):
                        checks[helper].assert_called_once_with(
                            qmp, fixture.actions[action_index][1], expected_timeout
                        )
                    checks["wait_for_screen_match"].assert_has_calls(
                        (
                            call(qmp, fixture.actions[-2][1], expected_timeout, False),
                            call(qmp, fixture.actions[-1][1], expected_timeout, True),
                        )
                    )
                    serial_timeout = (
                        DEFAULT_TIMEOUT if fixture_timeout is None else fixture_timeout
                    )
                    mocked["expect_prompt"].assert_any_call(
                        child, serial_timeout, "SERIAL 'true'"
                    )
                    mocked["wait_for_guest_file"].assert_called_once_with(
                        child, "/tmp/gui.ready", serial_timeout
                    )
                    if matched:
                        self.assertEqual(failures, [])
                        mocked["retain_visual_failure_diagnostics"].assert_not_called()
                    else:
                        self.assertEqual(len(failures), 6)
                        for failure in failures[:-1]:
                            self.assertIn(f"after {expected_timeout:.1f}s", failure)
                        mocked["retain_visual_failure_diagnostics"].assert_called_once()
                    child.close.assert_called_once_with(force=True)
                    qmp.close.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
