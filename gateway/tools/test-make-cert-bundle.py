#!/usr/bin/env python3
"""Tests of make-cert-bundle.py. Run it with: python3 gateway/tools/test-make-cert-bundle.py"""

import base64
import datetime
import hashlib
import importlib.util
import os
import struct
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CERTS = os.path.join(os.path.dirname(HERE), "certs")

spec = importlib.util.spec_from_file_location("make_cert_bundle", os.path.join(HERE, "make-cert-bundle.py"))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

# The bundle that the generator makes from the pinned list. The same bytes were made with the cryptography package, which is what the script of Espressif
# uses, and the two were equal. A change of this value means that the list or the generator changed, and it has to be looked at.
EXPECTED_BUNDLE_SHA256 = "8ca330673e7ecd07610eecfaffd23aba8b9ceca10a381d3878f268348edf5aec"
# The bundle that the build embeds, from the list of Mozilla and the file of extra roots. It was checked against the chain of the backend with openssl.
EXPECTED_FULL_BUNDLE_SHA256 = "cb82c296d50ef4ac58d193b4c27a7ce52f5a7e2d5986615b0239fa776349a787"
# The SHA-256 fingerprint of GlobalSign Root CA (R1), as GlobalSign publishes it. The server of the backend sends GTS Root R4 cross signed by this root.
GLOBALSIGN_R1_FINGERPRINT = "ebd41040e4bb3ec742c9e381d31ef2a41a48b6685c96e7cef3c1df6cd4331c99"
# The SHA-256 fingerprints of every certificate that certs/extra-roots.pem may hold. A person who adds a root of their own certificate authority adds its
# fingerprint here, so a root that nobody looked at cannot get into the bundle in the same step as its file.
EXTRA_ROOT_FINGERPRINTS = {GLOBALSIGN_R1_FINGERPRINT}


def read(name):
    with open(os.path.join(CERTS, name), "r", encoding="ascii", errors="replace") as handle:
        return handle.read()


def unique_entries(texts):
    """The distinct (subject, key) pairs of all the certificates in the texts. A root that two files both hold counts once."""
    found = set()
    for text in texts:
        for der in generator.pem_certificates(text):
            found.add(generator.subject_and_key(der))
    return found


def entries(bundle):
    count = struct.unpack(">H", bundle[:2])[0]
    pos = 2
    found = []
    for _ in range(count):
        name_length, key_length = struct.unpack(">HH", bundle[pos : pos + 4])
        pos += 4
        name = bundle[pos : pos + name_length]
        pos += name_length
        key = bundle[pos : pos + key_length]
        pos += key_length
        found.append((name, key))
    assert pos == len(bundle), "bytes are left after the last entry"
    return found


class PinnedList(unittest.TestCase):
    def test_the_list_is_the_one_that_is_pinned(self):
        with open(os.path.join(CERTS, "cacert.pem"), "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        with open(os.path.join(CERTS, "cacert.pem.sha256"), "r", encoding="ascii") as handle:
            pinned = handle.read().split()[0]
        self.assertEqual(pinned, digest)

    def test_the_bundle_of_the_list_is_the_expected_one(self):
        bundle = generator.make_bundle([read("cacert.pem")])
        self.assertEqual(EXPECTED_BUNDLE_SHA256, hashlib.sha256(bundle).hexdigest())

    def test_every_certificate_of_the_list_is_in_the_bundle_in_order_of_the_name(self):
        text = read("cacert.pem")
        bundle = generator.make_bundle([text])
        found = entries(bundle)
        self.assertEqual(text.count("-----BEGIN CERTIFICATE-----"), len(found))
        names = [name for name, _ in found]
        self.assertEqual(sorted(names), names)
        for name, key in found:
            self.assertEqual(0x30, name[0])
            self.assertEqual(0x30, key[0])

    def test_the_roots_of_the_chain_of_the_backend_are_in_the_bundle(self):
        found = entries(generator.make_bundle([read("cacert.pem")]))
        names = b"".join(name for name, _ in found)
        for root in (b"GTS Root R4", b"ISRG Root X1"):
            self.assertIn(root, names)


class ExtraRoots(unittest.TestCase):
    def test_the_file_of_extra_roots_is_the_one_that_is_pinned(self):
        with open(os.path.join(CERTS, "extra-roots.pem"), "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        with open(os.path.join(CERTS, "extra-roots.pem.sha256"), "r", encoding="ascii") as handle:
            pinned = handle.read().split()[0]
        self.assertEqual(pinned, digest)

    def test_the_file_of_extra_roots_holds_the_listed_roots_and_nothing_else(self):
        der_list = generator.pem_certificates(read("extra-roots.pem"))
        fingerprints = [hashlib.sha256(der).hexdigest() for der in der_list]
        self.assertEqual(sorted(EXTRA_ROOT_FINGERPRINTS), sorted(fingerprints))
        self.assertEqual(len(EXTRA_ROOT_FINGERPRINTS), len(read("extra-roots.pem").split("-----BEGIN CERTIFICATE-----")) - 1)

    def test_globalsign_r1_is_listed_and_is_the_root_that_it_claims_to_be(self):
        self.assertIn(GLOBALSIGN_R1_FINGERPRINT, EXTRA_ROOT_FINGERPRINTS)
        for der in generator.pem_certificates(read("extra-roots.pem")):
            if hashlib.sha256(der).hexdigest() == GLOBALSIGN_R1_FINGERPRINT:
                name, _ = generator.subject_and_key(der)
                self.assertIn(b"GlobalSign Root CA", name)
                self.assertEqual(name, generator.issuer_name(der))
                self.assertEqual(datetime.date(2028, 1, 28), generator.validity_end(der))

    def test_the_bundle_of_both_files_is_the_expected_one_and_holds_one_more_root(self):
        mozilla = read("cacert.pem")
        bundle = generator.make_bundle([mozilla, read("extra-roots.pem")])
        self.assertEqual(EXPECTED_FULL_BUNDLE_SHA256, hashlib.sha256(bundle).hexdigest())
        # The count follows the distinct roots of both files, so it stays right when Mozilla adds a root that the extra file holds.
        self.assertEqual(len(unique_entries([mozilla, read("extra-roots.pem")])), len(entries(bundle)))
        self.assertGreater(len(entries(bundle)), mozilla.count("-----BEGIN CERTIFICATE-----") - 1)

    def test_the_issuer_of_the_last_certificate_of_the_backend_is_in_the_bundle(self):
        found = entries(generator.make_bundle([read("cacert.pem"), read("extra-roots.pem")]))
        names = b"".join(name for name, _ in found)
        for root in (b"GlobalSign Root CA", b"GTS Root R4"):
            self.assertIn(root, names)

    def test_the_top_certificate_that_the_server_sends_has_its_issuer_in_the_bundle(self):
        # The server of the backend sends its own certificate, GTS WE1 and GTS Root R4 cross signed by GlobalSign Root CA. The core looks up the issuer of the
        # last one in the bundle. This is the certificate of the chain as the server sent it on 2026-11-11, and it ends on 2028-01-28. On that day the
        # list of Mozilla did not hold the issuer, and the hub failed every request until the extra root was added.
        with open(os.path.join(HERE, "fixtures", "gts-root-r4-cross-signed.pem"), "r", encoding="ascii") as handle:
            top = generator.pem_certificates(handle.read())[0]
        issuer = generator.issuer_name(top)
        names = [name for name, _ in entries(generator.make_bundle([read("cacert.pem"), read("extra-roots.pem")]))]
        self.assertIn(issuer, names)

    def test_the_end_of_an_extra_root_is_found_months_before_it(self):
        # Only GlobalSign R1 is looked at, so a root that a person adds to the file, with an end of its own, does not change this test.
        texts = [read("extra-roots.pem")]

        def globalsign(found):
            return [end for end, name in found if b"GlobalSign Root CA" in name]

        self.assertEqual([], globalsign(generator.roots_ending_soon(texts, datetime.date(2026, 11, 11), 183)))
        self.assertEqual([datetime.date(2028, 1, 28)], globalsign(generator.roots_ending_soon(texts, datetime.date(2027, 10, 1), 183)))
        self.assertEqual([datetime.date(2028, 1, 28)], globalsign(generator.roots_ending_soon(texts, datetime.date(2028, 2, 1), 183)))

    def test_two_certificates_with_one_subject_and_two_keys_are_refused(self):
        text = read("cacert.pem")
        der = generator.pem_certificates(text)[0]
        _, key = generator.subject_and_key(der)
        changed = bytearray(der)
        changed[der.find(key) + len(key) - 1] ^= 0x01
        pem = "-----BEGIN CERTIFICATE-----\n%s\n-----END CERTIFICATE-----\n" % base64.b64encode(bytes(changed)).decode("ascii")
        first = text.index("-----BEGIN CERTIFICATE-----")
        second = text.index("-----BEGIN CERTIFICATE-----", first + 1)
        with self.assertRaises(generator.BundleError):
            generator.make_bundle([text[first:second], pem])

    def test_a_root_that_two_different_files_both_hold_is_in_the_bundle_once(self):
        text = read("cacert.pem")
        starts = [text.index("-----BEGIN CERTIFICATE-----")]
        for _ in range(2):
            starts.append(text.index("-----BEGIN CERTIFICATE-----", starts[-1] + 1))
        file_a = text[starts[0] : starts[2]]  # roots 1 and 2
        file_b = text[starts[1] :]  # root 2 and all the others
        self.assertEqual(generator.make_bundle([text]), generator.make_bundle([file_a, file_b]))

    def test_a_root_that_two_files_both_hold_is_in_the_bundle_once(self):
        extra = read("extra-roots.pem")
        once = generator.make_bundle([read("cacert.pem"), extra])
        twice = generator.make_bundle([read("cacert.pem"), extra, extra])
        self.assertEqual(once, twice)


class Errors(unittest.TestCase):
    def test_an_input_without_a_certificate_is_refused(self):
        with self.assertRaises(generator.BundleError):
            generator.make_bundle(["nothing here"])
        with self.assertRaises(generator.BundleError):
            generator.make_bundle([])

    def test_a_block_that_is_not_base64_is_refused(self):
        with self.assertRaises(generator.BundleError):
            generator.make_bundle(["-----BEGIN CERTIFICATE-----\n!!!!\n-----END CERTIFICATE-----\n"])

    def test_a_certificate_that_is_cut_short_is_refused(self):
        text = read("cacert.pem")
        start = text.index("-----BEGIN CERTIFICATE-----")
        end = text.index("-----END CERTIFICATE-----", start)
        lines = text[start:end].split("\n")
        cut = "\n".join(lines[: len(lines) // 2]) + "\n-----END CERTIFICATE-----\n"
        with self.assertRaises(generator.BundleError):
            generator.make_bundle([cut])

    def test_a_block_that_lost_its_end_line_is_refused_and_not_left_out(self):
        text = read("cacert.pem")
        last_end = text.rindex("-----END CERTIFICATE-----")
        broken = text[:last_end] + text[last_end + len("-----END CERTIFICATE-----") :]
        with self.assertRaises(generator.BundleError):
            generator.make_bundle([broken])

    def test_bytes_that_are_not_a_certificate_are_refused(self):
        for body in ("AAAA", "MAA=", "MIIB", "MIGAAQ=="):
            with self.assertRaises(generator.BundleError):
                generator.make_bundle(["-----BEGIN CERTIFICATE-----\n" + body + "\n-----END CERTIFICATE-----\n"])

    def test_a_length_that_runs_past_the_certificate_is_refused(self):
        with self.assertRaises(generator.BundleError):
            generator.read_tlv(bytes([0x30, 0x82, 0xFF, 0xFF, 0x00]), 0)
        with self.assertRaises(generator.BundleError):
            generator.read_tlv(bytes([0x30, 0x85, 1, 2, 3, 4, 5]), 0)
        with self.assertRaises(generator.BundleError):
            generator.read_tlv(bytes([0x30, 0x80]), 0)
        with self.assertRaises(generator.BundleError):
            generator.read_tlv(bytes([0x30]), 0)


class Pin(unittest.TestCase):
    def check(self, pin_text, content=b"-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n"):
        with tempfile.TemporaryDirectory() as folder:
            list_path = os.path.join(folder, "list.pem")
            pin_path = os.path.join(folder, "list.pem.sha256")
            with open(list_path, "wb") as handle:
                handle.write(content)
            with open(pin_path, "w", encoding="ascii") as handle:
                handle.write(pin_text.replace("DIGEST", hashlib.sha256(content).hexdigest()))
            return generator.load_pinned_list(list_path, pin_path)

    def test_a_list_with_the_hash_that_the_pin_names_is_read(self):
        self.assertIn("BEGIN CERTIFICATE", self.check("DIGEST  list.pem\n"))
        self.assertIn("BEGIN CERTIFICATE", self.check("DIGEST\n"))

    def test_the_hash_may_be_in_capital_letters(self):
        digest = hashlib.sha256(b"-----BEGIN CERTIFICATE-----\n-----END CERTIFICATE-----\n").hexdigest().upper()
        self.assertIn("BEGIN CERTIFICATE", self.check(digest + "  list.pem\n"))

    def test_a_list_that_does_not_match_the_pin_is_refused(self):
        with self.assertRaises(generator.BundleError):
            self.check("0" * 64 + "  list.pem\n")
        with self.assertRaises(generator.BundleError):
            self.check("DIGEST"[:-1] + "  list.pem\n")

    def test_an_empty_pin_file_is_refused(self):
        for text in ("", "\n", "   \n"):
            with self.assertRaises(generator.BundleError):
                self.check(text)

    def test_a_list_that_changed_one_byte_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            list_path = os.path.join(folder, "list.pem")
            pin_path = os.path.join(folder, "list.pem.sha256")
            with open(list_path, "wb") as handle:
                handle.write(b"one")
            with open(pin_path, "w", encoding="ascii") as handle:
                handle.write(hashlib.sha256(b"two").hexdigest())
            with self.assertRaises(generator.BundleError):
                generator.load_pinned_list(list_path, pin_path)


class Output(unittest.TestCase):
    def test_the_same_input_gives_the_same_bytes_and_the_order_of_the_files_does_not_matter(self):
        text = read("cacert.pem")
        first = text.index("-----BEGIN CERTIFICATE-----")
        middle = text.index("-----BEGIN CERTIFICATE-----", first + 1)
        part_a = text[first:middle]
        part_b = text[middle:]
        self.assertEqual(generator.make_bundle([part_a, part_b]), generator.make_bundle([part_b, part_a]))
        self.assertEqual(generator.make_bundle([part_a, part_b]), generator.make_bundle([text]))


if __name__ == "__main__":
    unittest.main()
