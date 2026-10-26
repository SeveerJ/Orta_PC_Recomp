"""Second stage of Setup.exe, run with the Python it downloaded.

Fetches what the build needs -- the capstone disassembler for Python and the
llvm-mingw compiler -- into _downloads/ next to this folder (kept, so a retry
does not download them again), then runs build_orta.py.

Same "@@" protocol as build_orta.py.
"""

import hashlib
import json
import os
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DL = os.path.join(ROOT, "_downloads")
PY_DIR = os.path.dirname(sys.executable)
SITE = os.path.join(PY_DIR, "Lib", "site-packages")

CAPSTONE_MAJOR = 5                     # the toolkit is written against capstone 5
# The compiler release the build is tested with, fetched straight from its
# GitHub release (no API call) and checked against this hash.
LLVM_MINGW_TAG = "20251021"
LLVM_MINGW_ZIP = "llvm-mingw-%s-ucrt-x86_64.zip" % LLVM_MINGW_TAG
LLVM_MINGW_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/%s/%s" % (LLVM_MINGW_TAG, LLVM_MINGW_ZIP)
LLVM_MINGW_SHA256 = "0a0ce4733b54de8c4858ecfc2a08447de57728dccf9fbf7fcba3ce7e0d0c6af8"


def say(m):
    print(m, flush=True)


def progress(p, t):
    say("@@PROGRESS %d %s" % (p, t))


def fail(t):
    say("@@ERROR " + t)
    sys.exit(1)


def get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "OrtaPCSetup",
                                               "Accept": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def download(url, dest, label, p0, p1, sha256=None):
    if os.path.isfile(dest) and (not sha256 or file_sha256(dest) == sha256):
        say("Already downloaded: " + os.path.basename(dest))
        return dest
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "OrtaPCSetup"})
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got, last = 0, -1
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
                got += len(b)
                if total:
                    pct = p0 + (p1 - p0) * got // total
                    if pct != last:
                        progress(pct, "%s (%d of %d MB)" % (label, got >> 20, total >> 20))
                        last = pct
    except Exception as ex:
        fail("Downloading %s failed: %s. Check the internet connection and run Setup "
             "again; it carries on from here." % (label, ex))
    if sha256 and file_sha256(tmp) != sha256:
        os.remove(tmp)
        fail("The %s download was damaged. Run Setup again." % label)
    os.replace(tmp, dest)
    return dest


def file_sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def ensure_capstone():
    try:
        import capstone  # noqa: F401
        return
    except ImportError:
        pass
    progress(1, "Finding the disassembler")
    info = get_json("https://pypi.org/pypi/capstone/json")
    best = None
    for ver, files in info["releases"].items():
        try:
            major = int(ver.split(".")[0])
            key = tuple(int(x) for x in ver.split(".")[:3])
        except ValueError:
            continue
        if major != CAPSTONE_MAJOR:
            continue
        for f in files:
            if f["filename"].endswith("win_amd64.whl") and not f.get("yanked"):
                if best is None or key > best[0]:
                    best = (key, f)
    if not best:
        fail("Could not find the capstone disassembler for Windows.")
    f = best[1]
    whl = download(f["url"], os.path.join(DL, f["filename"]), "Downloading the disassembler",
                   1, 3, f["digests"]["sha256"])
    os.makedirs(SITE, exist_ok=True)
    with zipfile.ZipFile(whl) as z:
        z.extractall(SITE)
    say("capstone %s installed" % ".".join(map(str, best[0])))


def ensure_llvm_mingw():
    target = os.path.join(ROOT, "_llvm-mingw")
    for d in (os.listdir(target) if os.path.isdir(target) else []):
        bin_ = os.path.join(target, d, "bin")
        if os.path.isfile(os.path.join(bin_, "x86_64-w64-mingw32-clang.exe")):
            return bin_
    z = download(LLVM_MINGW_URL, os.path.join(DL, LLVM_MINGW_ZIP),
                 "Downloading the compiler", 5, 14, LLVM_MINGW_SHA256)
    progress(14, "Unpacking the compiler")
    with zipfile.ZipFile(z) as zf:
        zf.extractall(target)
    for d in os.listdir(target):
        bin_ = os.path.join(target, d, "bin")
        if os.path.isfile(os.path.join(bin_, "x86_64-w64-mingw32-clang.exe")):
            return bin_
    fail("The compiler did not unpack as expected.")


def main():
    args = sys.argv[1:]
    ensure_capstone()
    cc = ensure_llvm_mingw()
    # The build reports 1-100 for its own part; squeeze it into 15-100.
    p = subprocess.Popen([sys.executable, "-u", os.path.join(HERE, "build_orta.py"), "--cc", cc] + args,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         errors="replace", cwd=ROOT)
    for line in p.stdout:
        line = line.rstrip("\n")
        if line.startswith("@@PROGRESS "):
            parts = line.split(" ", 2)
            pct = 15 + int(parts[1]) * 85 // 100
            say("@@PROGRESS %d %s" % (pct, parts[2] if len(parts) > 2 else ""))
        else:
            say(line)
    sys.exit(p.wait())


if __name__ == "__main__":
    main()
