#!/usr/bin/env python3
"""
Make a title kit: everything a player needs to build the game module from
their own disc, and nothing taken from the disc.

The executable built with XBOXRECOMP_GAME_MODULE holds only the runtime
(src/kernel/recomp_module.h). The lifted code it loads is derived from the
game, so it cannot be handed out; what can be handed out is the recipe that
produces it. A kit is that recipe:

    kit.json            what to lift and how to compile it, the XBE it is for
                        (SHA-256), and the digest of the reference lift
    reference.json      SHA-256 of every generated file of the reference lift
    toolkit/            the lifter (tools/, scripts/recompile.py), the HLE
                        sources it scans for markers, the runtime header it
                        copies into the generated code, and the headers and
                        sources the module is compiled with
    title/              the title's seeds, its XDK symbol addresses,
                        recomp_manual.c and the headers that includes
    zig/                optional (--zig): a Zig distribution, the C compiler
                        scripts/build_module.py uses when it is there

scripts/build_module.py, run from the kit, does the rest on the player's
machine. The XDK symbols file is a list of names and addresses inside the
title's XBE (XbSymbolDatabase output); no bytes of the game are in the kit.

    python3 scripts/make_kit.py --project .. --xbe ../game_files/default.xbe \\
        --module-name futureperfect_game --game-name "..." --split 400 \\
        --seeds config/seeds/45410066.json --seeds ../config/seeds.json \\
        --out ../dist/kit

The reference lift is the project's src/recomp/gen as it is now: lift first,
then make the kit, so the two agree. The kit is refused when the generated
code was not lifted from this XBE or with these seeds (checked by lifting
again unless --no-verify).
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
from recompile import xbe_title_id  # noqa: E402
from build_module import gen_manifest, sha256_file  # noqa: E402

KIT_FORMAT = 1

# The toolkit's include directories the module is compiled with (the runtime
# targets' INTERFACE_INCLUDE_DIRECTORIES, see the title's CMakeLists.txt).
TOOLKIT_INCLUDES = ["include", "src", "src/input", "src/kernel", "src/config",
                    "src/platform", "src/platform/posix_include"]
# Compiled into every module: the ABI it was built for.
MODULE_SOURCES = ["src/kernel/recomp_module_abi.c"]
# The lifter's own packages; tests and caches are left out.
LIFTER_PACKAGES = ["tools/__init__.py", "tools/xbe_parser", "tools/disasm",
                   "tools/func_id", "tools/recomp"]


def manifest_digest(manifest):
    h = hashlib.sha256()
    for name in sorted(manifest):
        h.update(f"{name} {manifest[name]}\n".encode())
    return h.hexdigest()


def module_abi():
    text = (REPO / "src/kernel/recomp_module.h").read_text()
    return int(re.search(r"#define RECOMP_MODULE_ABI (\d+)", text).group(1))


def capstone_version():
    try:
        import capstone
        return capstone.__version__
    except ImportError:
        return None


def git_describe(path):
    try:
        return subprocess.run(["git", "-C", str(path), "describe", "--always",
                               "--dirty"], capture_output=True, text=True,
                              check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, keep):
    for p in sorted(src.rglob("*")):
        rel = p.relative_to(src)
        if p.is_file() and keep(rel):
            copy(p, dst / rel)


def lifter_file(rel):
    parts = rel.parts
    if "__pycache__" in parts or "output" in parts:
        return False
    if rel.name.startswith("test_") or rel.suffix == ".pyc":
        return False
    return True


def header_closure(cc, sources, include_dirs, defines):
    """Every header outside the system ones the sources include, by path."""
    argv = [cc, "-MM", "-std=gnu11", "-w"]
    argv += ["-D" + d for d in defines]
    argv += ["-I" + str(d) for d in include_dirs]
    found = set()
    for src in sources:
        out = subprocess.run(argv + [str(src)], capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f"{cc} -MM failed on {src}:\n{out.stderr}")
        for tok in out.stdout.replace("\\\n", " ").split()[1:]:
            if tok.endswith(".h"):
                found.add(Path(tok).resolve())
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True, help="The title project")
    ap.add_argument("--xbe", required=True, help="The XBE the reference lift is of")
    ap.add_argument("--module-name", required=True,
                    help="Module file name without extension (the name the "
                         "title's main.c loads)")
    ap.add_argument("--game-name", required=True)
    ap.add_argument("--split", type=int, required=True)
    ap.add_argument("--seeds", action="append", default=[], required=True)
    ap.add_argument("--hle-symbols", help="default: <xbe stem>_xdk_symbols.json")
    ap.add_argument("--extra-xbe-sha256", action="append", default=[],
                    help="Another XBE the kit accepts (it must lift to the "
                         "same code)")
    ap.add_argument("--zig", help="A Zig distribution directory to include")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"),
                    help="Compiler used to find the headers the module needs")
    ap.add_argument("--no-verify", action="store_true",
                    help="Do not lift again to check the reference")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    project = Path(args.project).resolve()
    xbe = Path(args.xbe).resolve()
    gen = project / "src" / "recomp" / "gen"
    manual = project / "src" / "recomp_manual.c"
    hle_symbols = (Path(args.hle_symbols) if args.hle_symbols
                   else xbe.with_name(xbe.stem + "_xdk_symbols.json")).resolve()
    out = Path(args.out).resolve()
    for need in (xbe, gen, manual, hle_symbols):
        if not need.exists():
            raise SystemExit(f"missing: {need}")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"{out} exists and is not empty")

    manifest = gen_manifest(gen)
    digest = manifest_digest(manifest)

    if not args.no_verify:
        # Lift again from scratch with exactly what goes into the kit, and
        # insist on the same result: a reference that the kit's own inputs do
        # not reproduce would fail every player's build.
        with tempfile.TemporaryDirectory(prefix="kit-verify-") as tmp:
            tmp = Path(tmp)
            argv = [sys.executable, str(REPO / "scripts/recompile.py"), str(xbe),
                    "--work-dir", str(tmp / "work"), "--gen-dir", str(tmp / "gen"),
                    "--json", str(tmp / "analysis.json"),
                    "--hle-symbols", str(hle_symbols),
                    "--project", str(project), "--split", str(args.split),
                    "--game-name", args.game_name]
            for s in args.seeds:
                argv += ["--seeds", str(Path(s).resolve())]
            print("verifying the reference lift (a full lift, a few minutes)...")
            env = dict(os.environ, PYTHONHASHSEED="0")
            r = subprocess.run(argv, cwd=REPO, env=env, capture_output=True, text=True)
            if r.returncode != 0:
                sys.stderr.write(r.stdout[-4000:] + r.stderr[-4000:])
                raise SystemExit("verification lift failed")
            again = gen_manifest(tmp / "gen")
            if again != manifest:
                diff = sorted(n for n in set(again) | set(manifest)
                              if again.get(n) != manifest.get(n))
                raise SystemExit(
                    f"{gen} is not what these inputs lift to ({len(diff)} "
                    f"files differ, e.g. {diff[:5]}). Lift again, then make "
                    f"the kit.")
        print("reference lift reproduced")

    toolkit = out / "toolkit"
    title = out / "title"

    # The lifter.
    for rel in LIFTER_PACKAGES:
        src = REPO / rel
        if src.is_dir():
            copy_tree(src, toolkit / rel, lifter_file)
        else:
            copy(src, toolkit / rel)
    copy(REPO / "scripts/recompile.py", toolkit / "scripts/recompile.py")
    copy(REPO / "scripts/build_module.py", toolkit / "scripts/build_module.py")
    copy(REPO / "templates/runtime/recomp_types.h",
         toolkit / "templates/runtime/recomp_types.h")
    # The lifter scans src/hle for HLE_EXPORT / HLE_IMPORT_VAR / HLE_ORIGINAL;
    # which replacements exist decides the thunks it writes.
    copy_tree(REPO / "src/hle", toolkit / "src/hle",
              lambda rel: rel.suffix in (".c", ".h"))
    for rel in MODULE_SOURCES:
        copy(REPO / rel, toolkit / rel)

    # The title.
    seeds = []
    for i, s in enumerate(args.seeds):
        name = f"seeds_{i}_{Path(s).name}"
        copy(Path(s).resolve(), title / name)
        seeds.append(f"title/{name}")
    # Without the path it was made from, which names the developer's disk.
    symbols = json.loads(hle_symbols.read_text())
    if isinstance(symbols, dict) and "xbe" in symbols:
        symbols["xbe"] = re.split(r"[\\/]", str(symbols["xbe"]))[-1]
    (title / "xdk_symbols.json").write_text(json.dumps(symbols, indent=1) + "\n")
    copy(manual, title / "src/recomp_manual.c")

    # Headers the module includes, wherever they live, at the same path
    # relative to the toolkit or the project.
    includes = [project / "src", project / "src/recomp"]
    includes += [REPO / d for d in TOOLKIT_INCLUDES]
    sources = [manual] + sorted(gen.glob("*.c")) + [REPO / s for s in MODULE_SOURCES]
    defines = ["_GNU_SOURCE", "NOMINMAX", "WIN32_LEAN_AND_MEAN"]
    headers = header_closure(args.cc, sources, includes + [gen], defines)
    for h in sorted(headers):
        if h.is_relative_to(gen):
            continue                    # generated: comes from the player's lift
        if h.is_relative_to(REPO):
            copy(h, toolkit / h.relative_to(REPO))
        elif h.is_relative_to(project):
            copy(h, title / h.relative_to(project))
        # else a system header

    def kit_rel(d):
        return (f"toolkit/{d.relative_to(REPO).as_posix()}" if d.is_relative_to(REPO)
                else f"title/{d.relative_to(project).as_posix()}")

    if args.zig:
        print("copying Zig...")
        shutil.copytree(Path(args.zig).resolve(), out / "zig", symlinks=True)

    xbe_sha = sha256_file(xbe)
    kit = {
        "format": KIT_FORMAT,
        "title_id": xbe_title_id(xbe),
        "game_name": args.game_name,
        "module_name": args.module_name,
        "module_abi": module_abi(),
        "xbe_sha256": [xbe_sha] + args.extra_xbe_sha256,
        "lift": {
            "split": args.split,
            "seeds": seeds,
            "hle_symbols": "title/xdk_symbols.json",
            "project": "title",
        },
        "compile": {
            "std": "gnu11",
            "defines": defines,
            "include_dirs": [kit_rel(d) for d in includes],
            "sources": ["title/src/recomp_manual.c"]
                       + [f"toolkit/{s}" for s in MODULE_SOURCES],
        },
        "reference": {"digest": digest, "files": len(manifest)},
        "made_from": {"toolkit": git_describe(REPO), "title": git_describe(project),
                      "python": sys.version.split()[0],
                      "capstone": capstone_version()},
    }
    (out / "kit.json").write_text(json.dumps(kit, indent=2) + "\n")
    (out / "reference.json").write_text(json.dumps(manifest, indent=1) + "\n")

    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"kit written to {out} ({size / 1e6:.1f} MB, reference {digest[:16]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
