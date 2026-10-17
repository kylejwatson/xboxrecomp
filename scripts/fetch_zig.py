#!/usr/bin/env python3
"""
Download a pinned Zig release for a title kit (scripts/make_kit.py --zig).

Zig is the C compiler a kit carries so a player can build the game module
without installing one: `zig cc` is clang with its own libc headers and
linker, in one ~50 MB archive per host, under the MIT licence.

    python3 scripts/fetch_zig.py --os linux --out build/zig-linux
    python3 scripts/fetch_zig.py --os windows --out build/zig-windows

The archive is checked against the SHA-256 in ziglang.org's release index
before anything is unpacked.

A Zig release can build for every target it knows (391 MB unpacked, most
of it libc headers and sources for other systems). A kit only ever builds
for the machine it runs on, so the unpacked copy is trimmed to that one
target: x86_64-linux-gnu on Linux, x86_64-windows-gnu (MinGW) on Windows.
--keep-all skips the trim; --strip-only DIR trims a copy already unpacked.
"""

import argparse
import hashlib
import io
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ZIG_VERSION = "0.16.0"
INDEX_URL = "https://ziglang.org/download/index.json"
HOSTS = {"linux": "x86_64-linux", "windows": "x86_64-windows"}

# What `zig cc -target <host>` reads, relative to lib/. Everything else
# under lib/ goes. Found by building a module (and, for Windows, a DLL)
# from an empty Zig cache with only these present: compiling C reads the
# clang headers and the target's libc headers; linking builds the target's
# libc pieces (glibc stubs from the ABI lists, or MinGW's CRT from source)
# and compiler_rt, which is Zig code and needs std.
KEEP_COMMON = ["include", "std", "compiler_rt", "compiler_rt.zig", "c", "c.zig",
               "zig.h", "ubsan_rt.zig"]
KEEP_TARGET = {
    "linux": ["libc/glibc", "libc/include/generic-glibc",
              "libc/include/x86-linux-gnu", "libc/include/x86-linux-any",
              "libc/include/any-linux-any"],
    "windows": ["libc/mingw", "libc/include/any-windows-any"],
}
# Inside what is kept, but for other architectures (MinGW's import-library
# definitions for 32-bit x86 and ARM).
DROP_TARGET = {
    "linux": [],
    "windows": ["libc/mingw/lib32", "libc/mingw/libarm32"],
}


def strip_zig(root, os_name):
    """Delete everything under root/lib/ that building for this host's own
    target does not read, and the documentation. Returns bytes freed."""
    root = Path(root)
    lib = root / "lib"
    keep = [lib / k for k in KEEP_COMMON + KEEP_TARGET[os_name]]
    for k in keep:
        if not k.exists():
            sys.exit(f"error: {k} missing: not the Zig layout this expects "
                     f"(Zig {ZIG_VERSION})")
    freed = 0

    def kept(p):
        return any(p == k or k in p.parents for k in keep)

    def needed(p):              # an ancestor of something kept
        return any(p in k.parents for k in keep)

    for p in sorted(lib.rglob("*"), key=lambda q: len(q.parts), reverse=True):
        if kept(p) or needed(p):
            continue
        if p.is_symlink() or p.is_file():
            freed += p.lstat().st_size
            p.unlink()
        elif p.is_dir():
            try:
                p.rmdir()
            except OSError:
                pass
    for d in DROP_TARGET[os_name]:
        d = lib / d
        if d.is_dir():
            freed += tree_size(d)
            shutil.rmtree(d)
    doc = root / "doc"
    if doc.is_dir():
        for p in sorted(doc.rglob("*"), key=lambda q: len(q.parts), reverse=True):
            if p.is_file():
                freed += p.stat().st_size
                p.unlink()
            else:
                p.rmdir()
        doc.rmdir()
    return freed


def tree_size(root):
    return sum(p.stat().st_size for p in Path(root).rglob("*") if p.is_file())


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--os", choices=sorted(HOSTS), required=True)
    ap.add_argument("--version", default=ZIG_VERSION)
    ap.add_argument("--out", help="directory to unpack into "
                    "(its contents: zig, lib/, ...)")
    ap.add_argument("--keep-all", action="store_true",
                    help="do not trim to the host's own target")
    ap.add_argument("--strip-only", metavar="DIR",
                    help="trim an unpacked Zig in place, download nothing")
    args = ap.parse_args()

    if args.strip_only:
        freed = strip_zig(args.strip_only, args.os)
        print(f"trimmed {args.strip_only} for {HOSTS[args.os]}: {freed / 1e6:.0f} MB "
              f"freed, {tree_size(args.strip_only) / 1e6:.0f} MB left")
        return 0
    if not args.out:
        ap.error("--out is required")

    out = Path(args.out).resolve()
    if (out / "zig").exists() or (out / "zig.exe").exists():
        print(f"{out} already holds Zig")
        return 0

    index = json.loads(fetch(INDEX_URL))
    try:
        entry = index[args.version][HOSTS[args.os]]
    except KeyError:
        sys.exit(f"error: no Zig {args.version} for {HOSTS[args.os]} in {INDEX_URL}")
    url, want = entry["tarball"], entry["shasum"]
    print(f"downloading {url} ({int(entry['size']) / 1e6:.0f} MB)...")
    data = fetch(url)
    have = hashlib.sha256(data).hexdigest()
    if have != want:
        sys.exit(f"error: SHA-256 mismatch for {url}: {have}, index says {want}")

    with tempfile.TemporaryDirectory(dir=out.parent if out.parent.exists() else None) as tmp:
        tmp = Path(tmp)
        if url.endswith(".zip"):
            zipfile.ZipFile(io.BytesIO(data)).extractall(tmp)
        else:
            with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tf:
                tf.extractall(tmp, filter="data")
        (top,) = [p for p in tmp.iterdir() if p.is_dir()]
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(top), str(out))
    if not args.keep_all:
        strip_zig(out, args.os)
    print(f"Zig {args.version} in {out} ({tree_size(out) / 1e6:.0f} MB"
          f"{'' if args.keep_all else ', trimmed to ' + HOSTS[args.os]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
