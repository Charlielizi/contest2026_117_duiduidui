# CA trust bundle

Pinned Mozilla CA extraction: 2026-09-25, https://curl.se/ca/cacert-2026-09-25.pem (https://curl.se/docs/caextract.html).

188,900 bytes; SHA-256 `a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`.

Derived from Mozilla's NSS root store, under Mozilla Public License 2.0: https://www.mozilla.org/en-US/MPL/2.0/. License and source notice are retained in the unmodified PEM header. Target path: `/resource/etc/ssl/curl/ca-certificates.crt`.

TASK-20261002-02. No private roots or device credentials are included. Parsing is bounded to 512 KiB and cached once per process; malformed/partially unsupported bundles fail closed. Target algorithm availability and peak heap use require hardware verification.
