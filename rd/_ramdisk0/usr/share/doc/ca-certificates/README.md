# CA certificates

Mario ships curl's [CA Extract](https://curl.se/docs/caextract.html) bundle of
Mozilla roots at `/etc/ssl/cert.pem`. The bundle is unmodified and licensed
under the Mozilla Public License 2.0 (see `LICENSE` in this directory).

- Snapshot: [cacert-2026-11-05.pem](https://curl.se/ca/cacert-2026-11-05.pem)
- SHA-256: `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`

Run `python3 util/update_ca_bundle.py` from the repository root to download the
latest dated PEM and `.sha256` from CA Extract, verify its SHA-256 digest and
certificates, and update `rd/_ramdisk0/etc/ssl/cert.pem` plus the snapshot and
checksum above (pass `--date YYYY-MM-DD` to pin a specific snapshot or `--check`
for a dry run). Rebuild the disk images and run `make net-test` afterwards.
Image builds use the checked-in snapshot and do not download certificates.
