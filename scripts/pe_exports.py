#!/usr/bin/env python3
"""
List the names a Windows executable or DLL exports, with no Visual Studio
tools (dumpbin) and no LLVM (llvm-nm): enough of the PE format to read the
export directory.

    python3 scripts/pe_exports.py futureperfect_recomp.exe
    python3 scripts/pe_exports.py --runtime-only futureperfect_recomp.exe

--runtime-only is the check before a build is handed out (make_kit.sh): the
executable must be a module host (it exports its runtime, recomp_module_load
among it) and must not define lifted code (no sub_XXXXXXXX). A build with the
game linked in exports nothing, so it fails too. Exit status 0 when it passes.
"""
import argparse
import re
import struct
import sys


def export_names(data):
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe, = struct.unpack_from("<I", data, 0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE file")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    opt_size, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    dirs = opt + (112 if magic == 0x20B else 96)   # PE32+ : PE32
    exp_rva, exp_size = struct.unpack_from("<II", data, dirs)
    if not exp_rva or not exp_size:
        return []
    sections = [struct.unpack_from("<8xIIII", data, opt + opt_size + 40 * i)
                for i in range(nsec)]

    def off(rva):
        for vsize, va, rsize, raw in sections:
            if va <= rva < va + max(vsize, rsize):
                return rva - va + raw
        raise ValueError(f"RVA {rva:#x} is in no section")

    e = off(exp_rva)
    count, = struct.unpack_from("<I", data, e + 24)
    names_rva, = struct.unpack_from("<I", data, e + 32)
    names = off(names_rva)
    out = []
    for i in range(count):
        p = off(struct.unpack_from("<I", data, names + 4 * i)[0])
        out.append(data[p:data.index(b"\0", p)].decode("ascii", "replace"))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--runtime-only", action="store_true",
                    help="check the file is a runtime-only module host")
    args = ap.parse_args()
    with open(args.file, "rb") as f:
        names = export_names(f.read())
    if not args.runtime_only:
        print("\n".join(names))
        return 0
    lifted = [n for n in names if re.fullmatch(r"sub_[0-9A-F]{8}", n)]
    if lifted:
        print(f"{args.file} exports {len(lifted)} lifted functions "
              f"(e.g. {lifted[0]})", file=sys.stderr)
        return 1
    if "recomp_module_load" not in names:
        print(f"{args.file} does not export the runtime (no recomp_module_load): "
              f"not a module-host build", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
