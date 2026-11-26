"""Serve the shared HTTP fixture over a restricted QEMU guest forward."""

import os
import socket
import ssl
import sys

from http_fixture import BODY, Handler

HTTPS_NAME = "https.mario.test"
WRONG_NAME = "wrong.mario.test"
TIMEOUT = 6


def server_context(cert, key):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(cert, key)
    return context


def assert_payload(connection):
    connection.request("GET", "/payload")
    response = connection.getresponse()
    assert response.status == 200, response.status
    assert response.getheader("X-Mario-Fixture") == "net"
    assert response.read() == BODY


def serve_forward(cert, key):
    with socket.socket(fileno=os.dup(sys.stdin.fileno())) as peer:
        peer.settimeout(TIMEOUT)
        try:
            with server_context(cert, key).wrap_socket(peer, server_side=True) as tls:
                Handler(tls, tls.getpeername(), None)
        except ssl.SSLError:
            pass


if __name__ == "__main__":
    serve_forward(*sys.argv[1:])
