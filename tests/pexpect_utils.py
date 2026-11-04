"""Small helpers for sending long text through pexpect-backed serial ports."""

from __future__ import annotations

import termios
import time


DEFAULT_CHUNK_SIZE = 256
DEFAULT_CHUNK_DELAY = 0.01


def make_pexpect_raw(child) -> None:
    """Disable canonical input on the host pseudo-terminal.

    pexpect's ``sendline()`` appends a newline and performs a single
    ``os.write()``.  The host pty is left in canonical mode by default, where
    Linux's ``N_TTY_BUF_SIZE`` limits a single input line to 4096 bytes.  Long
    commands and heredocs are therefore truncated before QEMU ever sees them.

    Clearing only ``ICANON`` removes that line-size limit without changing the
    echo/signal behavior that existing tests rely on.
    """

    attrs = termios.tcgetattr(child.child_fd)
    attrs[3] &= ~termios.ICANON
    termios.tcsetattr(child.child_fd, termios.TCSANOW, attrs)

    # The per-send delay would otherwise compound with the chunk delay below.
    child.delaybeforesend = None


def send_text(
    child,
    text: str,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    delay: float = DEFAULT_CHUNK_DELAY,
) -> None:
    """Send ``text`` in small chunks so the guest UART/TTY can drain."""

    for offset in range(0, len(text), chunk_size):
        child.send(text[offset : offset + chunk_size])
        if delay:
            time.sleep(delay)


def send_line(
    child,
    text: str = "",
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    delay: float = DEFAULT_CHUNK_DELAY,
) -> None:
    """Send ``text`` followed by a newline, using the same safe pacing."""

    send_text(child, text + "\n", chunk_size=chunk_size, delay=delay)
