"""Exercise the network monitor consumers in an isolated QEMU guest."""

import argparse
from pathlib import Path

from lit_tty_test import QEMU, TtyLitTest, run_test


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    test = TtyLitTest(Path(__file__).with_name("monitors.test")).parse()
    elapsed, failures, panel, diagnostics = run_test(
        test,
        QEMU,
        args.smp,
        0,
        network=(
            "-netdev",
            "user,id=monitor-net,restrict=on",
            "-device",
            "e1000,netdev=monitor-net,mac=52:54:00:a3:07:01",
        ),
    )
    if failures:
        raise SystemExit("\n".join([*failures, panel, diagnostics]))
    print(f"PASS: btop and System Monitor network (SMP={args.smp}) [{elapsed:.2f}s]")


if __name__ == "__main__":
    main()
