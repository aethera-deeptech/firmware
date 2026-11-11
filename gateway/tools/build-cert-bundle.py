"""PlatformIO script that runs before the build of the hub. It makes certs/x509_crt_bundle.bin from certs/cacert.pem and certs/extra-roots.pem, and the build
embeds the file.

The build stops when one of the two lists is not the file that its .sha256 file names. The pin guards against a damaged or half edited list. It does not guard
against a change of all of the files together, which a review has to read, see certs/README.md. The generated file is not committed.
"""

import datetime
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
    texts = []
    for name in ("cacert.pem", "extra-roots.pem"):
        texts.append(generator.load_pinned_list(os.path.join(certs_dir, name), os.path.join(certs_dir, name + ".sha256")))
    bundle = generator.make_bundle(texts)
    # The core does not check the dates of a root, but the certificates that a server sends have dates. A root of the extra file ends one day, and the
    # build says so for six months before, so nobody learns it from a hub that fails.
    for end, name in generator.roots_ending_soon([texts[1]], datetime.date.today(), 183):
        print("WARNING: an extra root certificate ends on %s (%s), see certs/README.md" % (end, name.decode("latin-1", errors="replace").strip()))
except (generator.BundleError, OSError, UnicodeDecodeError) as error:
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
