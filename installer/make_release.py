"""Assemble the release: one self-contained installer exe. Setup.exe with the
installer scripts and the source the build needs appended as a zip (then the
zip's size and "ORTAPAK1"). No game files, no translated game code.

    python installer/make_release.py <out.exe>
"""
import io
import os
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDE = [
    "xboxrecomp-main/src", "xboxrecomp-main/include", "xboxrecomp-main/tools",
    "xboxrecomp-main/templates", "xboxrecomp-main/LICENSE", "xboxrecomp-main/NOTICE",
    "xboxrecomp-main/LICENSES", "orta/src", "orta/third_party", "orta/seeds",
    "orta/package", "installer/setup_main.py", "installer/build_orta.py",
]
SKIP_DIRS = {"__pycache__", "output", "gen", "msvc-wine"}
SKIP_EXT = {".pyc", ".orig", ".ico"}



def main(out):
    n = 0
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for item in INCLUDE:
            p = os.path.join(ROOT, item)
            if os.path.isfile(p):
                z.write(p, item); n += 1
                continue
            for base, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
                for f in files:
                    if os.path.splitext(f)[1] in SKIP_EXT:
                        continue
                    full = os.path.join(base, f)
                    z.write(full, os.path.relpath(full, ROOT)); n += 1
    payload = buf.getvalue()
    with open(os.path.join(ROOT, "installer", "Setup.exe"), "rb") as f:
        stub = f.read()
    with open(out, "wb") as f:
        f.write(stub + payload + struct.pack("<Q", len(payload)) + b"ORTAPAK1")
    print("%s: %d files, %.1f MB" % (out, n, os.path.getsize(out) / 1e6))


if __name__ == "__main__":
    main(sys.argv[1])
