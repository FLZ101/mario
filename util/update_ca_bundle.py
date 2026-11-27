#!/usr/bin/env python3
"""Refresh Mario's bundled Mozilla CA roots at rd/_ramdisk0/etc/ssl/cert.pem."""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import ssl
import tempfile
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CERT_PATH = ROOT / "rd/_ramdisk0/etc/ssl/cert.pem"
DEFAULT_README_PATH = ROOT / "rd/_ramdisk0/usr/share/doc/ca-certificates/README.md"
CAEXTRACT_INDEX_URL = "https://curl.se/docs/caextract.html"
CAEXTRACT_BASE_URL = "https://curl.se/ca"
DATE_PATTERN = re.compile(r"^\d{4}-\d{2}-\d{2}$")
SNAPSHOT_HREF_PATTERN = re.compile(r"/ca/(cacert-(\d{4}-\d{2}-\d{2})\.pem)\b")
SHA256_LINE_PATTERN = re.compile(r"^([0-9a-fA-F]{64})(?:\s+\*?(\S+))?$")
PEM_CERT_PATTERN = re.compile(
    rb"-----BEGIN CERTIFICATE-----\r?\n.+?\r?\n-----END CERTIFICATE-----",
    re.DOTALL,
)
USER_AGENT = "mario-update-ca-bundle/1.0"
TIMEOUT_SECONDS = 30


def fetch_bytes(url: str, timeout: int = TIMEOUT_SECONDS) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    context = ssl.create_default_context()
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        return response.read()


def latest_snapshot_date(index_html: str) -> str:
    dates = sorted(
        {match.group(2) for match in SNAPSHOT_HREF_PATTERN.finditer(index_html)}
    )
    if not dates:
        raise ValueError(
            f"no dated cacert-YYYY-MM-DD.pem links found in {CAEXTRACT_INDEX_URL}"
        )
    return dates[-1]


def resolve_snapshot(
    date: str | None = None,
    url: str | None = None,
    sha256_url: str | None = None,
) -> tuple[str, str, str]:
    """Return (snapshot_name, pem_url, sha256_url)."""
    if url is not None:
        snapshot_name = url.rstrip("/").rsplit("/", 1)[-1]
        if not snapshot_name:
            raise ValueError(f"cannot derive snapshot filename from URL: {url}")
        return snapshot_name, url, sha256_url or f"{url}.sha256"

    if date is None:
        index_html = fetch_bytes(CAEXTRACT_INDEX_URL).decode("utf-8", errors="replace")
        date = latest_snapshot_date(index_html)
    elif not DATE_PATTERN.match(date):
        raise ValueError(f"invalid snapshot date {date!r}; expected YYYY-MM-DD")

    snapshot_name = f"cacert-{date}.pem"
    pem_url = f"{CAEXTRACT_BASE_URL}/{snapshot_name}"
    return snapshot_name, pem_url, sha256_url or f"{pem_url}.sha256"


def parse_sha256_file(text: str, expected_name: str | None = None) -> str:
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        match = SHA256_LINE_PATTERN.match(line)
        if not match:
            raise ValueError(f"malformed .sha256 line: {raw_line!r}")
        digest, filename = match.group(1).lower(), match.group(2)
        if (
            expected_name is not None
            and filename is not None
            and Path(filename).name != expected_name
        ):
            raise ValueError(
                f".sha256 filename {filename!r} does not match {expected_name!r}"
            )
        return digest
    raise ValueError(".sha256 file did not contain a digest")


def validate_ca_bundle(pem_bytes: bytes, expected_sha256: str) -> int:
    actual_sha256 = hashlib.sha256(pem_bytes).hexdigest()
    if actual_sha256 != expected_sha256.lower():
        raise ValueError(
            f"SHA-256 mismatch: expected {expected_sha256.lower()}, got {actual_sha256}"
        )
    cert_count = len(PEM_CERT_PATTERN.findall(pem_bytes))
    if cert_count == 0:
        raise ValueError("downloaded bundle contains no PEM certificates")

    with tempfile.NamedTemporaryFile(prefix="mario-cacert-", suffix=".pem") as handle:
        handle.write(pem_bytes)
        handle.flush()
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.load_verify_locations(cafile=handle.name)
        loaded = len(context.get_ca_certs())
        if loaded == 0:
            raise ValueError("OpenSSL loaded zero CA certificates from bundle")
    return cert_count


def update_readme_text(
    readme_text: str,
    snapshot_name: str,
    pem_url: str,
    sha256_hex: str,
) -> str:
    updated, snap_subs = re.subn(
        r"^- Snapshot: \[[^\]]+\]\([^)]+\)$",
        f"- Snapshot: [{snapshot_name}]({pem_url})",
        readme_text,
        count=1,
        flags=re.MULTILINE,
    )
    if snap_subs != 1:
        raise ValueError("failed to locate '- Snapshot: [...](...)' in CA README")

    updated, sha_subs = re.subn(
        r"^- SHA-256: `[0-9a-fA-F]{64}`$",
        f"- SHA-256: `{sha256_hex.lower()}`",
        updated,
        count=1,
        flags=re.MULTILINE,
    )
    if sha_subs != 1:
        raise ValueError("failed to locate '- SHA-256: `...`' in CA README")
    return updated


def atomic_write_bytes(target: Path, data: bytes, mode: int = 0o644) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{target.name}.", dir=target.parent)
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as handle:
            handle.write(data)
        os.chmod(tmp_path, mode)
        tmp_path.replace(target)
    except BaseException:
        tmp_path.unlink(missing_ok=True)
        raise


def update_bundle(
    *,
    cert_path: Path = DEFAULT_CERT_PATH,
    readme_path: Path | None = DEFAULT_README_PATH,
    date: str | None = None,
    url: str | None = None,
    sha256_url: str | None = None,
    check_only: bool = False,
) -> tuple[bool, str, str, int]:
    """Refresh or check the CA bundle; return (changed, snapshot_name, sha256, certs)."""
    snapshot_name, pem_url, resolved_sha256_url = resolve_snapshot(
        date=date,
        url=url,
        sha256_url=sha256_url,
    )
    sha256_text = fetch_bytes(resolved_sha256_url).decode("utf-8", errors="replace")
    expected_sha256 = parse_sha256_file(sha256_text, expected_name=snapshot_name)

    current_sha256 = (
        hashlib.sha256(cert_path.read_bytes()).hexdigest()
        if cert_path.is_file()
        else None
    )
    pem_bytes = fetch_bytes(pem_url)
    cert_count = validate_ca_bundle(pem_bytes, expected_sha256)

    new_readme: bytes | None = None
    readme_changed = False
    if readme_path is not None and readme_path.is_file():
        old_readme = readme_path.read_text(encoding="utf-8")
        rendered_readme = update_readme_text(
            old_readme,
            snapshot_name=snapshot_name,
            pem_url=pem_url,
            sha256_hex=expected_sha256,
        )
        readme_changed = rendered_readme != old_readme
        new_readme = rendered_readme.encode("utf-8")

    cert_changed = current_sha256 != expected_sha256
    changed = cert_changed or readme_changed
    if not check_only and changed:
        if cert_changed:
            atomic_write_bytes(cert_path, pem_bytes)
        if readme_changed and readme_path is not None and new_readme is not None:
            atomic_write_bytes(readme_path, new_readme)

    return changed, snapshot_name, expected_sha256, cert_count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument(
        "--date",
        metavar="YYYY-MM-DD",
        help="download a specific dated curl CA Extract snapshot",
    )
    source.add_argument(
        "--url",
        help="explicit PEM bundle URL (defaults to latest dated snapshot on curl.se)",
    )
    parser.add_argument(
        "--sha256-url",
        help="explicit .sha256 URL (defaults to <pem-url>.sha256)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=DEFAULT_CERT_PATH,
        help=f"destination PEM path (default: {DEFAULT_CERT_PATH})",
    )
    parser.add_argument(
        "--readme",
        type=Path,
        default=DEFAULT_README_PATH,
        help="CA certificates README to keep in sync",
    )
    parser.add_argument(
        "--no-readme",
        action="store_true",
        help="do not update the CA certificates README metadata",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="check whether a newer snapshot is available without writing files (exit 1 if outdated)",
    )
    args = parser.parse_args(argv)

    readme_path = None if args.no_readme else args.readme
    changed, snapshot_name, sha256_hex, cert_count = update_bundle(
        cert_path=args.output,
        readme_path=readme_path,
        date=args.date,
        url=args.url,
        sha256_url=args.sha256_url,
        check_only=args.check,
    )

    if args.check:
        if changed:
            print(
                f"update available: {snapshot_name} ({cert_count} certs, sha256={sha256_hex})"
            )
            return 1
        print(f"up to date: {snapshot_name} ({cert_count} certs, sha256={sha256_hex})")
        return 0

    status = "updated" if changed else "already up to date"
    print(
        f"{status}: {args.output} -> {snapshot_name} ({cert_count} certs, sha256={sha256_hex})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
