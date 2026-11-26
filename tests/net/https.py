"""Verify guest Python HTTPS in both directions on a restricted QEMU link."""

import argparse
import shlex
import ssl
import sys
import time
from contextlib import closing
from datetime import datetime, timedelta, timezone
from http.client import HTTPSConnection
from ipaddress import IPv4Address
from pathlib import Path
from tempfile import TemporaryDirectory

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

from boot import stage_fixture
from ethernet import SOURCES, finish, guest, listener, run, start, wait_status
from lit_kernel import kernel_artifacts
from lit_shell_test import require_files
from tls_fixture import HTTPS_NAME, TIMEOUT, assert_payload

HERE = Path(__file__).resolve().parent
PORT = 24680
GUEST = "/usr/bin/python -B /mnt/https_guest.py"


def certificates(directory):
    def name(common_name):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, common_name)])

    def builder(subject, issuer, key, start, end):
        return (
            x509.CertificateBuilder()
            .subject_name(subject)
            .issuer_name(issuer)
            .public_key(key.public_key())
            .serial_number(x509.random_serial_number())
            .not_valid_before(now + timedelta(days=start))
            .not_valid_after(now + timedelta(days=end))
            .add_extension(
                x509.SubjectKeyIdentifier.from_public_key(key.public_key()), False
            )
        )

    now = datetime.now(timezone.utc)
    ca_name = name("Mario HTTPS test CA")
    ca_key = ec.generate_private_key(ec.SECP256R1())
    ca_cert = (
        builder(ca_name, ca_name, ca_key, -30, 30)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            True,
        )
        .sign(ca_key, hashes.SHA256())
    )
    (directory / "ca.pem").write_bytes(ca_cert.public_bytes(serialization.Encoding.PEM))
    key = ec.generate_private_key(ec.SECP256R1())
    (directory / "server-key.pem").write_bytes(
        key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
    )
    for filename, end in (("server.pem", 1), ("expired.pem", -1)):
        cert = (
            builder(name(HTTPS_NAME), ca_name, key, -2, end)
            .add_extension(x509.BasicConstraints(ca=False, path_length=None), True)
            .add_extension(
                x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
                False,
            )
            .add_extension(
                x509.SubjectAlternativeName(
                    [x509.DNSName(HTTPS_NAME), x509.IPAddress(IPv4Address("127.0.0.1"))]
                ),
                False,
            )
            .add_extension(
                x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False
            )
            .sign(ca_key, hashes.SHA256())
        )
        (directory / filename).write_bytes(
            cert.public_bytes(serialization.Encoding.PEM)
        )


def forward(directory, port, cert):
    command = shlex.join(
        [
            sys.executable,
            str(HERE / "tls_fixture.py"),
            str(directory / cert),
            str(directory / "server-key.pem"),
        ]
    )
    return f",guestfwd=tcp:10.0.2.100:{port}-cmd:{command}"


def check_https(smp, directory, initrd):
    with listener() as reservation:
        inbound = reservation.getsockname()[1]
    backend = (
        f"user,id=wire,restrict=on,hostfwd=tcp:127.0.0.1:{inbound}-10.0.2.15:{PORT}"
        + forward(directory, PORT, "server.pem")
        + forward(directory, PORT + 1, "expired.pem")
    )
    with guest(smp, directory, initrd, backend) as (child, _monitor):
        run(child, "/sbin/netctl.exe dhcp start")
        wait_status(child, r"source=dhcp address=10\.0\.2\.15/24.*\r*\ndhcp=bound", 12)
        start(child, f"{GUEST} {PORT} {PORT + 1} {int(time.time())}")
        ready = child.expect(
            [r"(?m)^PY_TLS_READY\r*$", r"(?m)^ETH_STATUS=\d+\r*$"], timeout=90
        )
        assert ready == 0, child.before
        context = ssl.create_default_context(cafile=str(directory / "ca.pem"))
        with closing(
            HTTPSConnection("127.0.0.1", inbound, timeout=TIMEOUT, context=context)
        ) as connection:
            assert_payload(connection)
        assert "PY_TLS_OK" in finish(child)


def check_no_entropy(smp, directory, initrd):
    with guest(
        smp, directory, initrd, "user,id=wire,restrict=on", cpu="qemu32,-rdrand"
    ) as (child, _monitor):
        start(child, f"PYTHONHASHSEED=0 {GUEST} --no-entropy")
        output = finish(child, timeout=90)
        assert "PY_TLS_NO_ENTROPY_OK" in output, output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--smp", type=int, required=True)
    args = parser.parse_args()
    require_files(require_kernel=True)
    with kernel_artifacts(None), TemporaryDirectory(prefix="mario-https-") as temp:
        directory = Path(temp)
        certificates(directory)
        initrd = stage_fixture(
            directory,
            {"netctl": SOURCES["netctl"]},
            files=[
                HERE / filename
                for filename in (
                    "https_guest.py",
                    "python_guest.py",
                    "dns_fixture.py",
                    "http_fixture.py",
                    "tls_fixture.py",
                )
            ]
            + [
                directory / filename
                for filename in ("ca.pem", "server.pem", "server-key.pem")
            ],
        )
        check_https(args.smp, directory, initrd)
        check_no_entropy(args.smp, directory, initrd)
        print(f"PASS: Python HTTPS and entropy (SMP={args.smp})", flush=True)


if __name__ == "__main__":
    main()
