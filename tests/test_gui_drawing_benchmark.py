"""Host-only validation of GUI benchmark accounting and failure reporting."""

import json
import io
import unittest
from unittest.mock import patch

from PIL import Image

from bench_gui_drawing import (
    PAYLOAD_BYTES,
    WORK_CASES,
    enter_fullscreen,
    main,
    marker_bounds,
    parse_payload_size,
    parse_work_counters,
    run_samples,
    summarize,
)
from lit_gui_test import GuiTestError


class DrawingBenchmarkTests(unittest.TestCase):
    def test_summary_uses_nearest_rank_p95(self):
        result = summarize([float(index) for index in range(1, 21)])
        self.assertEqual(
            result,
            {"count": 20, "median_ms": 10500.0, "p95_ms": 19000.0, "max_ms": 20000.0},
        )

    def test_empty_summary_is_not_a_zero_latency_success(self):
        self.assertIsNone(summarize([])["median_ms"])
        self.assertEqual(summarize([])["count"], 0)

    def test_throughput_summary_does_not_rescale_bytes_per_second(self):
        self.assertEqual(
            summarize([10, 20, 30], scale=1, unit="bytes_per_second"),
            {
                "count": 3,
                "median_bytes_per_second": 20,
                "p95_bytes_per_second": 30,
                "max_bytes_per_second": 30,
            },
        )

    def test_payload_size_requires_one_exact_guest_result(self):
        self.assertEqual(
            parse_payload_size(
                f"echo command\r\n__GUI_BENCH_BYTES__{PAYLOAD_BYTES}\r\n"
            ),
            PAYLOAD_BYTES,
        )
        for output in (
            "",
            "__GUI_BENCH_BYTES__65\n",
            f"printf '__GUI_BENCH_BYTES__{PAYLOAD_BYTES}\\n'\n",
            f"__GUI_BENCH_BYTES__{PAYLOAD_BYTES}\n__GUI_BENCH_BYTES__{PAYLOAD_BYTES}\n",
        ):
            with self.subTest(output=output), self.assertRaises(GuiTestError):
                parse_payload_size(output)

    def test_nonfinite_or_nonpositive_timeout_is_rejected_before_boot(self):
        for timeout in ("nan", "inf", "-inf", "0", "-1"):
            arguments = [
                "bench_gui_drawing.py",
                f"--timeout={timeout}",
                "--output=/tmp/mario-invalid-benchmark.json",
            ]
            with self.subTest(timeout=timeout), patch("sys.argv", arguments), patch(
                "sys.stderr", new_callable=io.StringIO
            ), self.assertRaises(SystemExit) as error:
                main()
            self.assertEqual(error.exception.code, 2)

    def counters(self):
        return [
            {
                "case": name,
                "filled_pixels": 0,
                "blitted_pixels": 0,
                "flush_requests": 0,
                "writer_calls": 0,
                "written_bytes": 0,
                "presentations": 0,
            }
            for name in sorted(WORK_CASES)
        ]

    def encode(self, records):
        return "\r\n".join(
            "gui-drawing-work: " + json.dumps(record) for record in records
        )

    def test_all_work_cases_are_required(self):
        self.assertEqual(
            set(parse_work_counters(self.encode(self.counters()))), WORK_CASES
        )
        with self.assertRaises(ValueError):
            parse_work_counters(self.encode(self.counters()[:-1]))

    def test_duplicate_and_invalid_counters_fail(self):
        for value in (-1, 1.5, True):
            records = self.counters()
            records[0]["writer_calls"] = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_work_counters(self.encode(records))
        records = self.counters()
        with self.assertRaises(ValueError):
            parse_work_counters(self.encode(records + records[:1]))

    def test_marker_bounds_are_exact(self):
        image = Image.new("RGB", (10, 10))
        image.paste((19, 83, 137), (2, 3, 8, 9))
        self.assertEqual(marker_bounds(image, (19, 83, 137)), (2, 3, 8, 9))
        with self.assertRaises(GuiTestError):
            marker_bounds(image, (1, 2, 3))

    @patch("bench_gui_drawing.time.sleep")
    @patch("bench_gui_drawing.wait_visible")
    @patch("bench_gui_drawing.dump_screen")
    def test_warmups_are_separate_from_timed_samples(self, dump, wait, sleep):
        factory = lambda index: (lambda: None, lambda image: False)
        result = run_samples("fixture", factory, None, 3, 2, 1, 1024)
        self.assertEqual(len(result["warmups_seconds"]), 2)
        self.assertEqual(len(result["samples_seconds"]), 3)
        self.assertEqual(len(result["payload_bytes_per_second"]), 3)
        self.assertEqual(result["throughput_statistics"]["count"], 3)
        self.assertEqual(result["requested_samples"], 3)
        self.assertEqual(result["requested_warmups"], 2)
        self.assertFalse(result["failures"])

    @patch("bench_gui_drawing.dump_screen")
    def test_stale_visible_target_is_not_counted_as_fast_success(self, dump):
        factory = lambda index: (lambda: None, lambda image: True)
        result = run_samples("fixture", factory, None, 3, 0, 1)
        self.assertEqual(result["statistics"]["count"], 0)
        self.assertIn("already visible", result["failures"][0]["error"])
        self.assertIsNone(result["failures"][0]["elapsed_seconds"])

    @patch("bench_gui_drawing.time.monotonic", side_effect=[10.0, 11.5])
    @patch("bench_gui_drawing.wait_visible", side_effect=GuiTestError("timeout"))
    @patch("bench_gui_drawing.dump_screen")
    def test_timeouts_remain_failures(self, dump, wait, now):
        factory = lambda index: (lambda: None, lambda image: False)
        result = run_samples("fixture", factory, None, 3, 0, 1)
        self.assertEqual(result["statistics"]["count"], 0)
        self.assertEqual(
            result["failures"],
            [{"index": 0, "warmup": False, "error": "timeout", "elapsed_seconds": 1.5}],
        )

    @patch("bench_gui_drawing.wait_for_frame_advance")
    @patch("bench_gui_drawing.read_guest_frame_counter", return_value=8)
    @patch("bench_gui_drawing.wait_visible")
    @patch("bench_gui_drawing.send_keys")
    @patch("bench_gui_drawing.snapshot_guest_frame_counter", return_value=7)
    def test_fullscreen_setup_waits_for_visible_committed_geometry(
        self, snapshot, send, visible, read, advance
    ):
        order = []
        snapshot.side_effect = lambda *args: order.append("snapshot") or 7
        send.side_effect = lambda *args: order.append("inject")
        visible.side_effect = lambda *args: order.append("visible")
        advance.side_effect = lambda probe, *args: order.append("published") or probe()
        enter_fullscreen("child", "qmp", (19, 83, 137), 5)
        self.assertEqual(order, ["snapshot", "inject", "visible", "published"])
        read.assert_called_once_with("child", 5)
        image = Image.new("RGB", (32, 32))
        predicate = visible.call_args.args[1]
        self.assertFalse(predicate(image))
        image.putpixel((16, 16), (19, 83, 137))
        self.assertTrue(predicate(image))


if __name__ == "__main__":
    unittest.main()
