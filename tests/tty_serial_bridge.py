#!/usr/bin/env python3
"""Bridge a tmux pane's pseudo-terminal to a QEMU Unix serial socket."""

from __future__ import annotations

import argparse
import os
import selectors
import socket
import sys
import termios
import time
import tty


def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(fd, view)
        view = view[written:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("socket")
    args = parser.parse_args()

    stdin_fd = sys.stdin.fileno()
    stdout_fd = sys.stdout.fileno()
    original = termios.tcgetattr(stdin_fd)
    try:
        tty.setraw(stdin_fd)
        while True:
            serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                serial.connect(args.socket)
            except OSError:
                serial.close()
                time.sleep(0.05)
                continue

            try:
                serial.setblocking(False)
                with selectors.DefaultSelector() as selector:
                    selector.register(stdin_fd, selectors.EVENT_READ, "stdin")
                    selector.register(serial, selectors.EVENT_READ, "serial")
                    while True:
                        for key, _events in selector.select():
                            if key.data == "stdin":
                                data = os.read(stdin_fd, 4096)
                                if not data:
                                    return 0
                                serial.sendall(data)
                            else:
                                data = serial.recv(4096)
                                if not data:
                                    break
                                write_all(stdout_fd, data)
                        else:
                            continue
                        break
            except (BrokenPipeError, ConnectionError):
                pass
            finally:
                serial.close()
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, original)


if __name__ == "__main__":
    raise SystemExit(main())
