"""Host regressions for bounded GUI boot-failure diagnostics."""

from contextlib import ExitStack
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, call, patch

import pexpect
from PIL import Image

from lit_gui_test import (
    BOOT_DIAGNOSTIC_TIMEOUT,
    BOOT_PROMPT,
    BOOT_TIMEOUT,
    GuiTest,
    GuiTestError,
    Qmp,
    _run_test_locked,
    retain_boot_failure_diagnostics,
)


class GuiBootDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tempdir.name)
        fixture = self.root / "boot.test"
        fixture.write_text("EXIT:\n", encoding="utf-8")
        self.test = GuiTest(fixture).parse()

    def tearDown(self):
        self.tempdir.cleanup()

    def test_retains_screen_serial_status_and_every_cpu(self):
        class FakeQmp:
            def __init__(self):
                self.timeouts = []
                self.cpu = 0

            def connect(self, timeout):
                self.timeouts.append(timeout)

            def command(self, execute, arguments, *, timeout):
                self.timeouts.append(timeout)
                if execute == "query-status":
                    return {"status": "running"}
                if execute == "stop":
                    return {}
                if execute == "screendump":
                    Image.new("RGB", (2, 1), (4, 5, 6)).save(arguments["filename"])
                    return {}
                if execute == "query-cpus-fast":
                    return [{"cpu-index": 0}, {"cpu-index": 1}]
                if execute == "human-monitor-command":
                    command = arguments["command-line"]
                    if command.startswith("cpu "):
                        self.cpu = int(command.split()[1])
                        return ""
                    return f"CPU {self.cpu} EIP=00100000"
                raise AssertionError(execute)

        qmp = FakeQmp()
        directory = retain_boot_failure_diagnostics(
            self.test, qmp, "early boot output", 0, self.root / "out"
        )
        self.assertTrue((directory / "actual.ppm").is_file())
        self.assertEqual(
            (directory / "serial-tail.txt").read_text(encoding="latin-1"),
            "early boot output",
        )
        state = json.loads((directory / "qmp.json").read_text(encoding="utf-8"))
        self.assertEqual(state["query-status"], {"status": "running"})
        self.assertEqual(
            state["registers"],
            {"0": "CPU 0 EIP=00100000", "1": "CPU 1 EIP=00100000"},
        )
        self.assertFalse((directory / "diagnostic-errors.txt").exists())
        self.assertTrue(
            all(0 < timeout <= BOOT_DIAGNOSTIC_TIMEOUT for timeout in qmp.timeouts)
        )

    def test_all_diagnostic_requests_share_one_deadline(self):
        clock = [0.0]
        qmp = MagicMock()

        def connect(timeout):
            clock[0] += 3.0

        def command(execute, arguments, *, timeout):
            clock[0] += timeout
            raise GuiTestError("stalled QMP response")

        qmp.connect.side_effect = connect
        qmp.command.side_effect = command
        with patch("lit_gui_test.time.monotonic", side_effect=lambda: clock[0]):
            directory = retain_boot_failure_diagnostics(
                self.test, qmp, "", 0, self.root / "out"
            )
        qmp.connect.assert_called_once_with(timeout=BOOT_DIAGNOSTIC_TIMEOUT)
        qmp.command.assert_called_once_with("query-status", None, timeout=2.0)
        self.assertEqual(clock[0], BOOT_DIAGNOSTIC_TIMEOUT)
        errors = (directory / "diagnostic-errors.txt").read_text(encoding="utf-8")
        self.assertIn("stalled QMP response", errors)
        self.assertIn("boot diagnostic deadline exceeded", errors)
        self.assertTrue((directory / "serial-tail.txt").is_file())

    def test_retains_serial_when_qmp_is_unavailable(self):
        qmp = MagicMock()
        qmp.connect.side_effect = GuiTestError("QMP unavailable")
        directory = retain_boot_failure_diagnostics(
            self.test, qmp, "serial survives", 0, self.root / "out"
        )
        qmp.command.assert_not_called()
        self.assertEqual(
            (directory / "serial-tail.txt").read_text(encoding="latin-1"),
            "serial survives",
        )
        self.assertIn(
            "QMP unavailable",
            (directory / "diagnostic-errors.txt").read_text(encoding="utf-8"),
        )

    def test_boot_timeout_reports_phase_and_diagnostics_before_cleanup(self):
        directory = self.root / "out"
        with ExitStack() as stack:
            mocked = {
                name: stack.enter_context(patch(f"lit_gui_test.{name}"))
                for name in (
                    "pexpect.spawn",
                    "make_pexpect_raw",
                    "send_line",
                    "Qmp",
                    "time.sleep",
                    "os.unlink",
                    "retain_boot_failure_diagnostics",
                )
            }
            child = mocked["pexpect.spawn"].return_value
            qmp = mocked["Qmp"].return_value
            child.before = "early boot output"
            child.expect.side_effect = pexpect.exceptions.TIMEOUT("boot stalled")
            mocked["retain_boot_failure_diagnostics"].return_value = directory
            with self.assertRaisesRegex(
                GuiTestError, r"boot prompt timed out.*before fixture actions"
            ) as failure:
                _run_test_locked(self.test, "qemu", 7, 2)
        self.assertIn(str(directory), str(failure.exception))
        self.assertIn("early boot output", str(failure.exception))
        self.assertIsInstance(failure.exception.__cause__, pexpect.exceptions.TIMEOUT)
        child.expect.assert_called_once_with(BOOT_PROMPT, timeout=BOOT_TIMEOUT)
        mocked["retain_boot_failure_diagnostics"].assert_called_once_with(
            self.test, qmp, "early boot output", 7
        )
        mocked["send_line"].assert_not_called()
        child.close.assert_called_once_with(force=True)
        qmp.close.assert_called_once_with()


class QmpDeadlineTests(unittest.TestCase):
    def test_events_do_not_restart_the_command_timeout(self):
        qmp = Qmp("unused")
        qmp.socket = MagicMock()
        clock = [0.0]

        def receive(timeout):
            clock[0] += 2.0
            return {"event": "STOP"}

        with patch("lit_gui_test.time.monotonic", side_effect=lambda: clock[0]):
            with patch.object(qmp, "_read_message", side_effect=receive) as read:
                with self.assertRaisesRegex(GuiTestError, "query-status response"):
                    qmp.command("query-status", timeout=3.0)
        self.assertEqual(read.call_args_list, [call(3.0), call(1.0)])

    def test_connect_greeting_and_capabilities_share_one_deadline(self):
        qmp = Qmp("unused")
        clock = [0.0]
        connection = MagicMock()

        def connect(path):
            clock[0] += 1.0

        def greeting(timeout):
            clock[0] += 2.0
            return {"QMP": {}}

        connection.connect.side_effect = connect
        with patch("lit_gui_test.time.monotonic", side_effect=lambda: clock[0]):
            with patch("lit_gui_test.socket.socket", return_value=connection):
                with patch.object(qmp, "_read_message", side_effect=greeting) as read:
                    with patch.object(qmp, "command") as command:
                        qmp.connect(timeout=5.0)
        read.assert_called_once_with(4.0)
        command.assert_called_once_with("qmp_capabilities", timeout=2.0)

    def test_expired_command_does_not_send_a_request(self):
        qmp = Qmp("unused")
        qmp.socket = MagicMock()
        with self.assertRaisesRegex(GuiTestError, "query-status response"):
            qmp.command("query-status", timeout=0)
        qmp.socket.sendall.assert_not_called()


if __name__ == "__main__":
    unittest.main()
