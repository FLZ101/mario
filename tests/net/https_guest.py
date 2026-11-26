"""Exercise real TLS and certificate verification in the installed interpreter."""

import errno
import hashlib
import os
import ssl
import sys
import time
from contextlib import closing
from http.client import HTTPSConnection
from http.server import HTTPServer

from http_fixture import Handler
from python_guest import HTTP_IP, local_dns
from tls_fixture import HTTPS_NAME, TIMEOUT, WRONG_NAME, assert_payload, server_context


def check_no_entropy():
    with open("/dev/urandom", "rb", buffering=0) as device:
        for read in (os.urandom, device.read):
            try:
                read(32)
            except OSError as error:
                assert error.errno == errno.ENODEV, error
            else:
                raise AssertionError("entropy unexpectedly available without RDRAND")
    try:
        ssl.RAND_bytes(32)
    except ssl.SSLError:
        assert not ssl.RAND_status()
    else:
        raise AssertionError("TLS unexpectedly seeded without hardware entropy")
    print("PY_TLS_NO_ENTROPY_OK")


def reject(name, port, context, code):
    with closing(
        HTTPSConnection(name, port, timeout=TIMEOUT, context=context)
    ) as connection:
        try:
            connection.connect()
        except ssl.SSLCertVerificationError as error:
            assert error.verify_code == code, error
        else:
            raise AssertionError(f"verified invalid certificate for {name}:{port}")


def check_tls(port, expired_port, host_time):
    assert abs(time.time() - host_time) < 120, (time.time(), host_time)
    assert len(os.urandom(33)) == 33
    with open("/dev/urandom", "rb", buffering=0) as device:
        assert len(device.read(33)) == 33
    assert len(ssl.RAND_bytes(32)) == 32 and ssl.RAND_status()
    assert hashlib.sha256.__module__ == "_hashlib"
    paths = ssl.get_default_verify_paths()
    assert paths.cafile == "/etc/ssl/cert.pem", paths
    default = ssl.create_default_context()
    assert default.get_ca_certs(), "bundled CA certificates were not loaded"
    trusted = ssl.create_default_context(cafile="/mnt/ca.pem")
    with local_dns({HTTPS_NAME: HTTP_IP, WRONG_NAME: HTTP_IP}) as names:
        with closing(
            HTTPSConnection(HTTPS_NAME, port, timeout=TIMEOUT, context=trusted)
        ) as connection:
            assert_payload(connection)
        reject(WRONG_NAME, port, trusted, 62)
        reject(HTTPS_NAME, port, default, 20)
        reject(HTTPS_NAME, expired_port, trusted, 10)
        assert {HTTPS_NAME, WRONG_NAME} <= set(names), names

    context = server_context("/mnt/server.pem", "/mnt/server-key.pem")
    with HTTPServer(("10.0.2.15", port), Handler) as server:
        server.socket = context.wrap_socket(
            server.socket, server_side=True, do_handshake_on_connect=False
        )
        server.timeout = TIMEOUT
        print("PY_TLS_READY", flush=True)
        server.handle_request()
    print("PY_TLS_OK")


if __name__ == "__main__":
    if sys.argv[1:] == ["--no-entropy"]:
        check_no_entropy()
    else:
        check_tls(*map(int, sys.argv[1:]))
