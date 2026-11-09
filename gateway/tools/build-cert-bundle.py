"""PlatformIO script that runs before the build of the hub. It makes certs/x509_crt_bundle.bin from certs/cacert.pem, and the build embeds the file.

The build stops when cacert.pem is not the file that certs/cacert.pem.sha256 names, so a change of the list of trusted roots always goes through a change of
that hash, which a review sees. The generated file is not committed.
"""

import hashlib
import importlib.util
import os

Import("env")  # noqa: F821 (SCons gives this name to the script)

project_dir = env["PROJECT_DIR"]  # noqa: F821
certs_dir = os.path.join(project_dir, "certs")
list_path = os.path.join(certs_dir, "cacert.pem")
pin_path = os.path.join(certs_dir, "cacert.pem.sha256")
output_path = os.path.join(certs_dir, "x509_crt_bundle.bin")

spec = importlib.util.spec_from_file_location("make_cert_bundle", os.path.join(project_dir, "tools", "make-cert-bundle.py"))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

with open(list_path, "rb") as handle:
    raw = handle.read()
with open(pin_path, "r", encoding="ascii") as handle:
    pinned = handle.read().split()[0]
if hashlib.sha256(raw).hexdigest() != pinned:
    raise SystemExit("certs/cacert.pem is not the file that certs/cacert.pem.sha256 names, so the bundle of root certificates is not made")

bundle = generator.make_bundle([raw.decode("ascii", errors="replace")])
current = None
if os.path.exists(output_path):
    with open(output_path, "rb") as handle:
        current = handle.read()
if current != bundle:
    with open(output_path, "wb") as handle:
        handle.write(bundle)
