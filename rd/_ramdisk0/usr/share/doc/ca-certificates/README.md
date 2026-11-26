# CA certificates

Mario ships curl's [CA Extract](https://curl.se/docs/caextract.html) bundle of
Mozilla roots at `/etc/ssl/cert.pem`. The bundle is unmodified and licensed
under the Mozilla Public License 2.0 (see `LICENSE` in this directory).

- Snapshot: [cacert-2026-11-05.pem](https://curl.se/ca/cacert-2026-11-05.pem)
- SHA-256: `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`

To refresh the bundle, download a dated PEM and its `.sha256` file from CA
Extract. Verify it with `sha256sum -c`, replace
`rd/_ramdisk0/etc/ssl/cert.pem` in the source tree, and update the snapshot and
checksum above. Rebuild the disk images and run the network tests. Image builds
use the checked-in snapshot and do not download certificates.
