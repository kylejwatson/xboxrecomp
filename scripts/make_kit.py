#!/usr/bin/env python3
"""
Make a title kit: everything a player needs to build the game module from
their own disc, and nothing taken from the disc.

The executable built with XBOXRECOMP_GAME_MODULE holds only the runtime
(src/kernel/recomp_module.h). The lifted code it loads is derived from the
game, so it cannot be handed out; what can be handed out is the recipe that
produces it. A kit is that recipe:

    kit.json            what to lift and how to compile it, the XBE it is for
                        (SHA-256), the digest of the reference lift, and the
                        kit id (a digest of everything below but zig/ and
                        python/, whose versions it records)
    kit.id              the kit id alone, for the runtime: it names the
                        directory the module is built into
    reference.json      SHA-256 of every generated file of the reference lift
    toolkit/            the lifter (tools/, scripts/recompile.py), the HLE
                        sources it scans for markers, the runtime header it
                        copies into the generated code, and the headers and
                        sources the module is compiled with
    title/              the title's seeds, its XDK symbol addresses,
                        recomp_manual.c and the headers that includes
    zig/                optional (--zig): a Zig distribution, the C compiler
                        scripts/build_module.py uses when it is there
    python/             optional (--python): a relocatable Python with capstone
                        (scripts/fetch_python.py), which the runtime runs
                        build_module.py with, so the player needs no Python.
                        Run make_kit.py with that same interpreter: the check
                        that the lift reproduces the reference then uses it.

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

Without the disc (a CI runner): what the kit takes from the XBE and the lift
-- the XBE's SHA-256 and title ID, the digest of every generated file, the
XDK symbol addresses -- is facts about the disc, not bytes of it, and
--record-inputs DIR writes it down whenever a kit is made with the disc.
--inputs DIR (instead of --xbe) makes the same kit from those files alone:
nothing is lifted or verified, and the headers the module needs are found
from recomp_manual.c and the toolkit sources (the generated code includes
nothing outside src/recomp/gen).
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
if sys.platform == "win32":
    # The real <windows.h>: the POSIX stand-ins are for the other hosts.
    TOOLKIT_INCLUDES.remove("src/platform/posix_include")
# Compiled into every module: the ABI it was built for.
MODULE_SOURCES = ["src/kernel/recomp_module_abi.c"]
# The lifter's own packages; tests and caches are left out.
LIFTER_PACKAGES = ["tools/__init__.py", "tools/xbe_parser", "tools/disasm",
                   "tools/func_id", "tools/recomp",
                   # build_module.py reads default.xbe out of a disc image
                   "tools/xiso"]


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


def header_closure(cc, sources, include_dirs, defines, generated_missing=False):
    """Every header outside the system ones the sources include, by path.
    generated_missing: the generated headers are not there (no lift); they
    are left out, and they include nothing that is not generated."""
    # cc may be a command with arguments ("zig cc -target ...").
    argv = cc.split() + ["-MM", "-std=gnu11", "-w"] + (["-MG"] if generated_missing else [])
    argv += ["-D" + d for d in defines]
    argv += ["-I" + str(d) for d in include_dirs]
    found = set()
    for src in sources:
        out = subprocess.run(argv + [str(src)], capture_output=True, text=True)
        if out.returncode != 0:
            raise SystemExit(f"{cc} -MM failed on {src}:\n{out.stderr}")
        for tok in out.stdout.replace("\\\n", " ").split()[1:]:
            if tok.endswith(".h") and (not generated_missing or
                                       ("/" in tok or "\\" in tok)
                                       and Path(tok).exists()):
                found.add(Path(tok).resolve())
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--project", required=True, help="The title project")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--xbe", help="The XBE the reference lift is of")
    src.add_argument("--inputs", help="A --record-inputs directory, instead of the "
                                      "XBE and the lift (no disc needed)")
    ap.add_argument("--record-inputs", help="Write what the kit takes from the disc "
                                            "and the lift into this directory")
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
    ap.add_argument("--python", help="A fetch_python.py directory (holding python/) "
                                     "to include")
    ap.add_argument("--link-lib", action="append", default=[],
                    help="A library the module links (Windows: the runtime "
                         "executable's import library), copied into kit/lib")
    ap.add_argument("--define", action="append", default=[],
                    help="A preprocessor define the module is built with, as "
                         "the title's build has it (e.g. RECOMP_REGS_IN_MODULE)")
    ap.add_argument("--cc", default=os.environ.get("CC", "cc"),
                    help="Compiler used to find the headers the module needs")
    ap.add_argument("--no-verify", action="store_true",
                    help="Do not lift again to check the reference")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    project = Path(args.project).resolve()
    gen = project / "src" / "recomp" / "gen"
    manual = project / "src" / "recomp_manual.c"
    out = Path(args.out).resolve()
    if args.inputs:
        inputs = Path(args.inputs).resolve()
        hle_symbols = inputs / "xdk_symbols.json"
        recorded = json.loads((inputs / "inputs.json").read_text())
        needs = (manual, hle_symbols)
    else:
        xbe = Path(args.xbe).resolve()
        hle_symbols = (Path(args.hle_symbols) if args.hle_symbols
                       else xbe.with_name(xbe.stem + "_xdk_symbols.json")).resolve()
        needs = (xbe, gen, manual, hle_symbols)
    for need in needs:
        if not need.exists():
            raise SystemExit(f"missing: {need}")
    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"{out} exists and is not empty")

    if args.inputs:
        manifest = recorded["reference"]
        xbe_sha, title_id = recorded["xbe_sha256"], recorded["title_id"]
        print(f"from recorded inputs ({inputs}): the reference is not checked")
    else:
        manifest = gen_manifest(gen)
        xbe_sha, title_id = sha256_file(xbe), xbe_title_id(xbe)
    digest = manifest_digest(manifest)

    if not args.no_verify and not args.inputs:
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
    sources = [manual] + [REPO / s for s in MODULE_SOURCES]
    if not args.inputs:
        sources += sorted(gen.glob("*.c"))
    defines = ["_GNU_SOURCE", "NOMINMAX", "WIN32_LEAN_AND_MEAN"] + args.define
    headers = header_closure(args.cc, sources, includes + [gen], defines,
                             generated_missing=bool(args.inputs))
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
    if args.python:
        print("copying Python...")
        # Without bytecode caches: running the source Python leaves them, and
        # they would differ from one kit to the next.
        shutil.copytree(Path(args.python).resolve() / "python", out / "python",
                        symlinks=True,
                        ignore=shutil.ignore_patterns("__pycache__"))

    # Libraries the module links: on Windows the executable's import library,
    # through which the module reaches the runtime (Linux resolves those
    # imports against the executable at load).
    link = []
    for lib in args.link_lib:
        copy(Path(lib), out / "lib" / Path(lib).name)
        link.append(f"lib/{Path(lib).name}")

    kit = {
        "format": KIT_FORMAT,
        "title_id": title_id,
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
        **({"link": link} if link else {}),
        "reference": {"digest": digest, "files": len(manifest)},
        "made_from": {"toolkit": git_describe(REPO), "title": git_describe(project),
                      "python": sys.version.split()[0],
                      "capstone": capstone_version()},
    }
    if args.zig:
        zig = out / "zig" / ("zig.exe" if (out / "zig" / "zig.exe").exists() else "zig")
        kit["zig"] = subprocess.run([str(zig), "version"], capture_output=True,
                                    text=True).stdout.strip() or None
    if args.python:
        py = out / "python" / ("python.exe" if (out / "python" / "python.exe").exists()
                               else "bin/python3")
        ver = subprocess.run([str(py), "-c", "import sys, capstone; "
                              "print(sys.version.split()[0], capstone.__version__)"],
                             capture_output=True, text=True).stdout.split()
        if len(ver) != 2:
            sys.exit(f"make_kit: {py} does not run, or has no capstone")
        kit["python"] = {"version": ver[0], "capstone": ver[1]}
        if kit["python"] != {"version": kit["made_from"]["python"],
                             "capstone": kit["made_from"]["capstone"]}:
            print(f"warning: the kit's Python ({ver[0]}, capstone {ver[1]}) is not "
                  "the one this kit was made with, so the reference was not "
                  "checked with it; run make_kit.py with the bundled interpreter")
    reference_text = json.dumps(manifest, indent=1) + "\n"
    (out / "reference.json").write_text(reference_text)
    # The kit id: everything that decides what the module is. A runtime that
    # carries this kit builds into a directory named after it, so a changed
    # override or seed is a new module, never a stale one reused.
    # made_from is a note of where the kit came from, not an input: the same
    # sources from a git checkout and from a plain copy (CI) are one kit.
    h = hashlib.sha256()
    h.update(json.dumps({k: v for k, v in kit.items() if k != "made_from"},
                        sort_keys=True).encode())
    h.update(reference_text.encode())
    for part in ("toolkit", "title"):
        for f in sorted((out / part).rglob("*")):
            if f.is_file():
                h.update(f.relative_to(out).as_posix().encode() + b"\0")
                h.update(hashlib.sha256(f.read_bytes()).digest())
    kit["kit_id"] = h.hexdigest()
    (out / "kit.json").write_text(json.dumps(kit, indent=2) + "\n")
    (out / "kit.id").write_text(kit["kit_id"] + "\n")
    if args.record_inputs and not args.inputs:
        rec = Path(args.record_inputs)
        rec.mkdir(parents=True, exist_ok=True)
        (rec / "inputs.json").write_text(json.dumps(
            {"xbe_sha256": xbe_sha, "title_id": title_id, "reference": manifest},
            indent=1, sort_keys=True) + "\n")
        shutil.copy2(title / "xdk_symbols.json", rec / "xdk_symbols.json")
        print(f"recorded the disc's facts in {rec}")

    size = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"kit written to {out} ({size / 1e6:.1f} MB, reference {digest[:16]}, "
          f"kit id {kit['kit_id'][:16]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
