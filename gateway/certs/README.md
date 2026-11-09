# Root certificates of the hub

The hub checks the certificate of the backend against a bundle of root certificates. Nothing in the firmware switches that check off.

## Files

- `cacert.pem` is the list of root certificates of Mozilla, as curl publishes it at https://curl.se/ca/cacert.pem. The list is dated 2026-11-05. It is made of public certificates and holds no secret. Mozilla publishes it under the Mozilla Public License 2.0.
- `cacert.pem.sha256` is the SHA-256 of that file, as curl publishes it at https://curl.se/ca/cacert.pem.sha256. The build of the hub stops when the file and the hash do not match.
- `x509_crt_bundle.bin` is made by the build from `cacert.pem` with `../tools/make-cert-bundle.py`. It is not committed. The build embeds it in the firmware, and the program reads it through `_binary_certs_x509_crt_bundle_bin_start`.

## Why a bundle of our own

The Arduino-ESP32 core 2.0.17 has no bundle file, and `WiFiClientSecure::setCACertBundle` needs one. The generator of Espressif needs the Python package `cryptography`, so the repository has a generator that uses the standard library only. Its output is the same bytes as the output of the computation of Espressif (the subject of each certificate and its public key as DER, sorted by the subject), and the test pins that result.

## How to renew the list

1. Download the new `cacert.pem` and its `.sha256` file from curl.se, and check that the hash matches.
2. Put both files here, and run `python3 gateway/tools/make-cert-bundle.py gateway/certs/cacert.pem -o /dev/null`. It prints the SHA-256 of the bundle. Put that value in `EXPECTED_BUNDLE_SHA256` in `gateway/tools/test-make-cert-bundle.py`, and run the test.
3. Open a pull request. The review sees the new hash of the list and the new hash of the bundle.

A hub gets a renewed list with a new firmware, as every other change. The chain of the backend ends today at GTS Root R4, which is valid until 2036, and the list holds it. The test checks that this root and ISRG Root X1 stay in the list.
