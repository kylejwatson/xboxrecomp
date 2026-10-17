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


def fetch(url):
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--os", choices=sorted(HOSTS), required=True)
    ap.add_argument("--version", default=ZIG_VERSION)
    ap.add_argument("--out", required=True, help="directory to unpack into "
                    "(its contents: zig, lib/, ...)")
    args = ap.parse_args()

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
    print(f"Zig {args.version} in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
