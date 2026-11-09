#!/usr/bin/env python3
"""Makes the bundle of root certificates that WiFiClientSecure::setCACertBundle takes.

The format is the one of the file x509_crt_bundle of ESP-IDF, as esp_crt_bundle.c of the Arduino-ESP32 core 2.0.17 reads it. It starts with the number of
certificates as 2 bytes, big endian. Each certificate follows as the length of its subject name (2 bytes, big endian), the length of its public key (2 bytes,
big endian), the subject name and the public key. The name is the DER encoding of the subject of the certificate (the whole sequence, with its tag and
length), and the key is the DER encoding of its SubjectPublicKeyInfo. The entries are sorted by the bytes of the name, because the core finds a root with a
binary search on the issuer of the first certificate in the chain that it does not trust yet.

The program uses the standard library only, so the build needs no package. It does not check signatures, dates or the purpose of a certificate, because
the file that it reads (the list of Mozilla) is the decision of what is trusted, and the hash of that file is pinned next to it.

Usage: make-cert-bundle.py INPUT.pem [INPUT.pem ...] --output OUTPUT
"""

import argparse
import base64
import binascii
import hashlib
import re
import struct
import sys

MAX_ENTRY_PART = 0xFFFF
MAX_CERTIFICATES = 0xFFFF
PEM_BLOCK = re.compile(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", re.DOTALL)

TAG_SEQUENCE = 0x30
TAG_VERSION = 0xA0


class BundleError(Exception):
    """The input is not a list of certificates that the bundle can hold."""


def read_tlv(data, pos):
    """Reads the header of one DER element at pos and returns (tag, start of the content, end of the content)."""
    if pos + 2 > len(data):
        raise BundleError("the certificate is cut short")
    tag = data[pos]
    first = data[pos + 1]
    pos += 2
    if first < 0x80:
        length = first
    else:
        count = first & 0x7F
        if count == 0 or count > 4 or pos + count > len(data):
            raise BundleError("the length of an element is not valid")
        length = int.from_bytes(data[pos : pos + count], "big")
        pos += count
    end = pos + length
    if end > len(data):
        raise BundleError("an element is longer than the certificate")
    return tag, pos, end


def subject_and_key(der):
    """Returns the DER of the subject name and of the SubjectPublicKeyInfo of one certificate."""
    tag, start, end = read_tlv(der, 0)
    if tag != TAG_SEQUENCE or end != len(der):
        raise BundleError("the certificate is not one sequence")
    tag, pos, tbs_end = read_tlv(der, start)  # tbsCertificate
    if tag != TAG_SEQUENCE:
        raise BundleError("the certificate has no tbsCertificate")
    tag, _, element_end = read_tlv(der, pos)
    if tag == TAG_VERSION:
        pos = element_end  # the version is optional and is not needed
    # serial number, signature algorithm, issuer, validity, subject, subject public key info
    fields = []
    for _ in range(6):
        tag, _, element_end = read_tlv(der, pos)
        if element_end > tbs_end:
            raise BundleError("a field is longer than the tbsCertificate")
        fields.append((pos, element_end, tag))
        pos = element_end
    subject, key = fields[4], fields[5]
    if subject[2] != TAG_SEQUENCE or key[2] != TAG_SEQUENCE:
        raise BundleError("the subject or the key is not a sequence")
    return der[subject[0] : subject[1]], der[key[0] : key[1]]


def pem_certificates(text):
    """Returns the DER of every certificate in the text, in the order of the text."""
    found = []
    for block in PEM_BLOCK.finditer(text):
        body = "".join(block.group(1).split())
        try:
            found.append(base64.b64decode(body, validate=True))
        except (binascii.Error, ValueError) as error:
            raise BundleError("a certificate is not valid base64") from error
    return found


def make_bundle(texts):
    """Makes the bundle from the texts of PEM files, and returns its bytes."""
    entries = []
    for text in texts:
        for der in pem_certificates(text):
            entries.append(subject_and_key(der))
    if not entries:
        raise BundleError("there is no certificate in the input")
    if len(entries) > MAX_CERTIFICATES:
        raise BundleError("there are too many certificates")
    entries.sort()
    out = bytearray(struct.pack(">H", len(entries)))
    for name, key in entries:
        if len(name) > MAX_ENTRY_PART or len(key) > MAX_ENTRY_PART:
            raise BundleError("a name or a key is too long")
        out += struct.pack(">HH", len(name), len(key))
        out += name
        out += key
    return bytes(out)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Makes the bundle of root certificates for WiFiClientSecure")
    parser.add_argument("inputs", nargs="+", help="PEM files with the certificates")
    parser.add_argument("--output", "-o", required=True, help="the file to write")
    args = parser.parse_args(argv)
    texts = []
    for path in args.inputs:
        with open(path, "r", encoding="ascii", errors="replace") as handle:
            texts.append(handle.read())
    try:
        bundle = make_bundle(texts)
    except BundleError as error:
        sys.stderr.write("make-cert-bundle: %s\n" % error)
        return 1
    with open(args.output, "wb") as handle:
        handle.write(bundle)
    count = struct.unpack(">H", bundle[:2])[0]
    sys.stderr.write("make-cert-bundle: %d certificates, %d bytes, sha256 %s\n" % (count, len(bundle), hashlib.sha256(bundle).hexdigest()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
