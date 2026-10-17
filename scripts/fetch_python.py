#!/usr/bin/env python3
"""
Download a pinned, relocatable Python with capstone for a title kit
(scripts/make_kit.py --python).

The kit's lifter is Python and disassembles with capstone, and a player
should not have to install either: a kit carries its own. The interpreter
is python-build-standalone's (the CPython builds uv uses: unpacked anywhere,
no system Python needed), capstone is the PyPI wheel unpacked into its
site-packages. Both are pinned by SHA-256 below and checked before anything
is unpacked.

    python3 scripts/fetch_python.py --os linux --out build-python-linux
    python3 scripts/fetch_python.py --os windows --out build-python-windows

The result is <out>/python/ (bin/python3 on Linux, python.exe on Windows).
It is trimmed of what a lifter never imports (tests, Tk, IDLE, pip, headers,
the static library); --keep-all skips that.

The capstone version is the one the reference lifts are made with: a kit
refuses a lift that differs from its reference, and capstone decides how
every instruction reads. Change it only together with a new reference.
"""

import argparse
import hashlib
import io
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

PY_RELEASE = "20260929"
PY_VERSION = "3.13.15"
CAPSTONE_VERSION = "5.0.9"

PY_ASSETS = {
    "linux": ("x86_64-unknown-linux-gnu",
              "c552deaf9ed0e678dee4ccc760d6dcd7cab33f1bd43e8037123b4a4db8508e14"),
    "windows": ("x86_64-pc-windows-msvc",
                "d11d0d4b4fefa5df09e925dbd37600457f5488a9712a82f3f0dd1ff9431c2ccf"),
}
CAPSTONE_WHEELS = {
    "linux": ("https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/"
              "capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl",
              "273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e"),
    "windows": ("https://files.pythonhosted.org/packages/50/e6/6f06fdb6a9ed32b2f7cd9c036b92d5324112c3ef7080f2c71efc367d40dd/"
                "capstone-5.0.9-py3-none-win_amd64.whl",
                "732cedbbb56d42e723f14d7af6387f1454194a820b4b96b56d1e53f865ef85d0"),
}

PY_MINOR = ".".join(PY_VERSION.split(".")[:2])

# Relative to python/. The standard library is under lib/python3.13 on Linux
# and Lib on Windows; {lib} stands for it.
DROP = ["include", "share", "bin/idle3", f"bin/idle{PY_MINOR}", "bin/pydoc3",
        f"bin/pydoc{PY_MINOR}", "bin/pip", "bin/pip3", f"bin/pip{PY_MINOR}",
        "lib/pkgconfig", "lib/itcl4", "lib/tcl9", "lib/tcl9.0", "lib/tk9.0",
        "lib/thread3", "tcl", "Scripts", "libs",
        "{lib}/test", "{lib}/idlelib", "{lib}/tkinter", "{lib}/turtledemo",
        "{lib}/ensurepip", "{lib}/pydoc_data", "{lib}/site-packages/pip",
        f"{{lib}}/config-{PY_MINOR}-x86_64-linux-gnu"]
DROP_GLOBS = ["lib/libtcl*", "lib/libtk*", "{lib}/site-packages/pip-*",
              "DLLs/_tkinter.pyd", "DLLs/tcl*.dll", "DLLs/tk*.dll"]


def fetch(url):
    with urllib.request.urlopen(url, timeout=300) as r:
        return r.read()


def checked(url, want):
    data = fetch(url)
    have = hashlib.sha256(data).hexdigest()
    if have != want:
        sys.exit(f"fetch_python: {url}\n  SHA-256 {have}\n  expected {want}")
    return data


def tree_size(root):
    # Symlinks once: bin/python3 and the libpython links point at files.
    return sum(p.stat().st_size for p in Path(root).rglob("*")
               if p.is_file() and not p.is_symlink())


def stdlib_dir(py, os_name):
    return py / ("Lib" if os_name == "windows" else f"lib/python{PY_MINOR}")


def strip_python(py, os_name):
    lib = stdlib_dir(py, os_name).relative_to(py).as_posix()
    for rel in DROP:
        p = py / rel.replace("{lib}", lib)
        if p.is_dir():
            shutil.rmtree(p)
        elif p.exists():
            p.unlink()
    for pattern in DROP_GLOBS:
        for p in py.glob(pattern.replace("{lib}", lib)):
            shutil.rmtree(p) if p.is_dir() else p.unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--os", choices=sorted(PY_ASSETS), required=True)
    ap.add_argument("--out", required=True, help="Directory to create; python/ goes in it")
    ap.add_argument("--keep-all", action="store_true", help="Do not trim")
    args = ap.parse_args()

    out = Path(args.out)
    if out.exists():
        sys.exit(f"fetch_python: {out} exists")
    triple, sha = PY_ASSETS[args.os]
    name = f"cpython-{PY_VERSION}+{PY_RELEASE}-{triple}-install_only_stripped.tar.gz"
    url = (f"https://github.com/astral-sh/python-build-standalone/releases/download/"
           f"{PY_RELEASE}/{name.replace('+', '%2B')}")
    print(f"fetching {name}...")
    data = checked(url, sha)
    out.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        tf.extractall(out, filter="tar")
    py = out / "python"
    if not py.is_dir():
        sys.exit(f"fetch_python: {name} did not unpack to python/")

    wurl, wsha = CAPSTONE_WHEELS[args.os]
    print(f"fetching capstone {CAPSTONE_VERSION}...")
    wheel = checked(wurl, wsha)
    site = stdlib_dir(py, args.os) / "site-packages"
    site.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(wheel)) as zf:
        zf.extractall(site)
    for so in site.glob("capstone/lib/*"):
        so.chmod(0o755)

    if not args.keep_all:
        before = tree_size(py)
        strip_python(py, args.os)
        print(f"trimmed {before / 2**20:.0f} -> {tree_size(py) / 2**20:.0f} MB")
    print(f"python {PY_VERSION} + capstone {CAPSTONE_VERSION} in {py}")


if __name__ == "__main__":
    main()
