# Root certificates of the hub

The hub checks the certificate of the backend against a bundle of root certificates. Nothing in the firmware switches that check off.

## Files

- `cacert.pem` is the list of root certificates of Mozilla, as curl publishes it at https://curl.se/ca/cacert.pem. The list is dated 2026-11-05. It is made of public certificates and holds no secret. Mozilla publishes it under the Mozilla Public License 2.0 (https://www.mozilla.org/MPL/2.0/). The file is kept here unchanged, and the source of every certificate in it is the list of Mozilla (https://wiki.mozilla.org/CA). The rest of the repository is under the GPL 3.0, which the MPL 2.0 allows for a larger work.
- `cacert.pem.sha256` is the SHA-256 of that file, as curl publishes it at https://curl.se/ca/cacert.pem.sha256. The build of the hub stops when the file and the hash do not match. The pin guards against a damaged list and against a change of one of the files only. A change of the list, its hash and the expected bundle hash in one pull request passes the build, and a review has to check the list against curl.se, which a hash cannot do for the reader.
- `extra-roots.pem` holds the roots that the hub trusts on top of the list of Mozilla. Each one has its subject, its fingerprint, its end of life and the reason written above it in the file. Today it holds `GlobalSign Root CA` (R1), see the next section. `extra-roots.pem.sha256` is its SHA-256, and the build stops when the file and the hash do not match, as for the list of Mozilla.
- `x509_crt_bundle.bin` is made by the build from `cacert.pem` and `extra-roots.pem` with `../tools/make-cert-bundle.py`. A root that is in both files is in the bundle once. The file is not committed. The build embeds it in the firmware, and the client of the backend reads it through `_binary_certs_x509_crt_bundle_bin_start`.

## Why GlobalSign Root CA is an extra root

The server of the backend (checked with `openssl s_client` on 2026-11-11) sends three certificates, which are its own, `GTS WE1`, and `GTS Root R4` cross signed by `GlobalSign Root CA`. The check of the core looks for the issuer of the last certificate that the server sends, and not for a root that is higher up. That issuer is `GlobalSign Root CA`, which Mozilla no longer lists, so the first build of the hub with only the list of Mozilla failed every request with `Failed to verify certificate` (seen on a board on 2026-11-11). The root was downloaded from https://secure.globalsign.com/cacert/root-r1.crt, its SHA-256 fingerprint is the one that GlobalSign publishes, and `openssl verify` accepts the cross signed certificate of the server with it. The test of the generator pins the fingerprint.

The root is valid until 2028-01-28. Look at it again before that date. A root can vouch for any site, so this one widens what the hub trusts by one well known root that Mozilla dropped.

## A backend with its own certificate authority

A person who hosts their own backend can add the root of its certificate authority to `extra-roots.pem` (the text of a PEM certificate, with a short comment above it), put the new SHA-256 of the file in `extra-roots.pem.sha256`, and put the new hash of the bundle in `EXPECTED_FULL_BUNDLE_SHA256` in `gateway/tools/test-make-cert-bundle.py`. The build and the test then use the new file. A hub trusts only what its own firmware holds, so such a person builds and flashes their own firmware.

## Why a bundle of our own

The Arduino-ESP32 core 2.0.17 has no bundle file, and `WiFiClientSecure::setCACertBundle` needs one. The generator of Espressif needs the Python package `cryptography`, so the repository has a generator that uses the standard library only. Its output is the same bytes as the output of the computation of Espressif (the subject of each certificate and its public key as DER, sorted by the subject), and the test pins that result.

## How to renew the list

1. Download the new `cacert.pem` and its `.sha256` file from curl.se, and check that the hash matches.
2. Put both files here, and run `python3 gateway/tools/make-cert-bundle.py gateway/certs/cacert.pem -o /dev/null`. It prints the SHA-256 of the bundle. Put that value in `EXPECTED_BUNDLE_SHA256` in `gateway/tools/test-make-cert-bundle.py`. Run the same program with `gateway/certs/extra-roots.pem` added to get the value for `EXPECTED_FULL_BUNDLE_SHA256`, and run the test.
3. Open a pull request. The review sees the new hash of the list and the new hash of the bundle.

A hub gets a renewed list with a new firmware, as every other change. A hub that is switched off for years and wakes up after the chain of the backend has changed cannot learn the new roots by itself, and it needs an update that is checked by its signature and not by this bundle. That update is planned and is not built yet. The test checks that `GTS Root R4`, `ISRG Root X1` and `GlobalSign Root CA` stay in the bundle.
