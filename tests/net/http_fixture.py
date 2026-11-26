"""Shared HTTP fixture for the isolated host and guest tests."""

from http.server import BaseHTTPRequestHandler
from time import sleep

BODY = bytes(range(256)) * 4


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def log_message(self, *_args):
        pass

    def do_GET(self):
        if self.path == "/slow":
            sleep(1.5)
            return
        if self.path != "/payload":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(BODY)))
        self.send_header("X-Mario-Fixture", "net")
        self.end_headers()
        self.wfile.write(BODY)


def serve_forward():
    import signal
    import sys

    class ForwardedHandler(Handler):
        def setup(self):
            self.rfile = sys.stdin.buffer
            self.wfile = sys.stdout.buffer

        def finish(self):
            self.wfile.flush()

    signal.alarm(5)
    ForwardedHandler(None, ("127.0.0.1", 0), None)


if __name__ == "__main__":
    serve_forward()
