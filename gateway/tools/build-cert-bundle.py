"""PlatformIO script that runs before the build of the hub. It makes certs/x509_crt_bundle.bin from certs/cacert.pem, and the build embeds the file.

The build stops when cacert.pem is not the file that certs/cacert.pem.sha256 names. The pin guards against a damaged or half edited list. It does not guard
against a change of all of the files together, which a review has to read, see certs/README.md. The generated file is not committed.
"""

import importlib.util
import os

Import("env")  # noqa: F821 (SCons gives this name to the script)

project_dir = env["PROJECT_DIR"]  # noqa: F821
certs_dir = os.path.join(project_dir, "certs")
output_path = os.path.join(certs_dir, "x509_crt_bundle.bin")

spec = importlib.util.spec_from_file_location("make_cert_bundle", os.path.join(project_dir, "tools", "make-cert-bundle.py"))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)

try:
    text = generator.load_pinned_list(os.path.join(certs_dir, "cacert.pem"), os.path.join(certs_dir, "cacert.pem.sha256"))
    bundle = generator.make_bundle([text])
except generator.BundleError as error:
    raise SystemExit("the bundle of root certificates is not made, because %s" % error)

current = None
if os.path.exists(output_path):
    with open(output_path, "rb") as handle:
        current = handle.read()
if current != bundle:
    # A temporary file and a rename, so a build that is killed never leaves half a bundle for the next one to embed.
    temporary = output_path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(bundle)
    os.replace(temporary, output_path)
