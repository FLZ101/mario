"""Host-side regressions for the GUI stress fixture's reclamation barrier."""

from pathlib import Path
import subprocess
import unittest

from lit_gui_test import GuiTest


class GuiStressReclamationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture = Path(__file__).parent / "gui" / "gui-stress.test"
        test = GuiTest(fixture).parse()
        script = next(
            value
            for action, value in test.actions
            if action == "serial-script" and "/tmp/gui-stress-helpers" in value
        )
        cls.helper = (
            script.split("<<'FUNCTIONS'\n", 1)[1]
            .rsplit("\nFUNCTIONS", 1)[0]
            .replace("/usr/bin/test/gui-cleanup.exe --pty-pool", "stress_pty_pool")
        )

    def run_reclamation(self, **overrides):
        environment = {
            "WM_DELAY": "0",
            "SHELL_DELAY": "0",
            "TERMINAL_DELAY": "0",
            "PTY_DELAY": "0",
            "PTY_OUTPUT": "",
            "CURRENT_WM_PID": "100",
            "CURRENT_SHELL_PID": "200",
        }
        environment.update({name: str(value) for name, value in overrides.items()})
        script = """
wm_pid=100
shell_pid=200
wm_descriptors=4
shell_descriptors=5
pty_capacity=256
sleep_count=0
ls() {
    case "$1" in
        /proc/100/fd) descriptor_count=$wm_descriptors; delay=$WM_DELAY ;;
        /proc/200/fd) descriptor_count=$shell_descriptors; delay=$SHELL_DELAY ;;
        *) return 1 ;;
    esac
    if test "$sleep_count" -lt "$delay"; then
        descriptor_count=$((descriptor_count + 1))
    fi
    descriptor=0
    while test "$descriptor" -lt "$descriptor_count"; do
        echo "$descriptor"
        descriptor=$((descriptor + 1))
    done
}
pgrep() {
    case "$1" in
        mario-wm) echo "$CURRENT_WM_PID" ;;
        mario-desktop) echo "$CURRENT_SHELL_PID" ;;
        terminal)
            if test "$sleep_count" -lt "$TERMINAL_DELAY"; then
                echo 300
            fi
            ;;
    esac
}
stress_pty_pool() {
    if test -n "$PTY_OUTPUT"; then
        echo "$PTY_OUTPUT"
        return 1
    fi
    if test "$sleep_count" -lt "$PTY_DELAY"; then
        echo "$((pty_capacity - 1))"
    else
        echo "$pty_capacity"
    fi
}
sleep() {
    sleep_count=$((sleep_count + 1))
}
"""
        script += (
            self.helper
            + """
stress_reclaimed
result=$?
echo "sleeps=$sleep_count"
exit "$result"
"""
        )
        return subprocess.run(
            ["/bin/sh"],
            input=script,
            text=True,
            capture_output=True,
            env=environment,
            timeout=5,
        )

    def test_accepts_reclaimed_baseline_without_sleep(self):
        result = self.run_reclamation()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sleeps=0\n", result.stdout)

    def test_waits_for_each_resource_to_be_reclaimed(self):
        for resource in ("WM_DELAY", "SHELL_DELAY", "TERMINAL_DELAY", "PTY_DELAY"):
            with self.subTest(resource=resource):
                result = self.run_reclamation(**{resource: 2})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("sleeps=2\n", result.stdout)

    def test_waits_until_all_resources_are_reclaimed(self):
        result = self.run_reclamation(
            WM_DELAY=1, SHELL_DELAY=2, TERMINAL_DELAY=3, PTY_DELAY=4
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("sleeps=4\n", result.stdout)

    def test_bounds_wait_for_each_unreclaimed_resource(self):
        for resource in ("WM_DELAY", "SHELL_DELAY", "TERMINAL_DELAY", "PTY_DELAY"):
            with self.subTest(resource=resource):
                result = self.run_reclamation(**{resource: 100})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("baseline not restored", result.stdout)
                self.assertIn("terminal processes:", result.stdout)
                self.assertIn("available PTYs", result.stdout)
                self.assertIn("sleeps=9\n", result.stdout)

    def test_rejects_replacement_wm_or_desktop(self):
        for process in ("CURRENT_WM_PID", "CURRENT_SHELL_PID"):
            with self.subTest(process=process):
                result = self.run_reclamation(**{process: 999})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_rejects_failed_pty_capacity_probe(self):
        result = self.run_reclamation(PTY_OUTPUT="unavailable")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("baseline not restored", result.stdout)
        self.assertIn("sleeps=9\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
