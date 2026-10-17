#!/usr/bin/env python3
"""
Build a title's game module from the player's own XBE, using a title kit
(scripts/make_kit.py).

    python3 kit/toolkit/scripts/build_module.py --xbe /path/to/default.xbe

The steps:

  1. the XBE's SHA-256 must be one the kit was made for;
  2. the four pipeline stages (scripts/recompile.py) lift it into the work
     directory, with the kit's seeds, XDK symbols and recomp_manual.c;
  3. the generated code must match the kit's reference lift file for file,
     because the title's overrides are written against exactly that code;
  4. the generated code, recomp_manual.c and the ABI stamp are compiled and
     linked into <module_name>.so (.dll on Windows).

The compiler is, in order: --cc; $RECOMP_MODULE_CC; the kit's own Zig
(kit/zig/zig, `zig cc`); a `zig` on PATH; clang; gcc. Objects are kept in
the work directory and only recompiled when their source or the flags
change, so a second build after a kit update compiles what changed.

Exit status 0 means the module was written. Every failure prints one line
starting "error:" that a launcher can show as it is.
"""

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from pathlib import Path

KIT_FORMAT = 1
HERE = Path(__file__).resolve()
DEFAULT_KIT = HERE.parent.parent.parent     # kit/toolkit/scripts/ -> kit/


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def say(msg):
    print(msg, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def gen_file_digest(path):
    """SHA-256 of a generated file with CRLF read as LF: the lifter writes
    text in the host's line endings, and a Windows and a Linux lift of the
    same XBE are otherwise identical."""
    return hashlib.sha256(Path(path).read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def gen_manifest(gen_dir):
    """{file name: digest} for a generated-code directory."""
    return {p.name: gen_file_digest(p) for p in sorted(Path(gen_dir).iterdir())
            if p.is_file()}


def default_work_dir(kit):
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache"))
    return base / "xboxrecomp" / "module-build" / (kit["title_id"] or "title")


def module_suffix():
    return ".dll" if sys.platform == "win32" else ".so"


# ── compiler ────────────────────────────────────────────────────────────────

def find_compiler(kit_dir, requested):
    """[argv prefix], description. Zig is used as `zig cc -target ...`."""
    target = {"Linux": "x86_64-linux-gnu", "Windows": "x86_64-windows-gnu"}.get(
        platform.system())

    def zig(path):
        argv = [str(path), "cc"]
        if target:
            argv += ["-target", target]
        return argv, f"zig ({path})"

    cand = requested or os.environ.get("RECOMP_MODULE_CC")
    if cand:
        argv = cand.split()
        if Path(argv[0]).name.startswith("zig") and argv[1:2] != ["cc"]:
            return zig(argv[0])
        return argv, cand
    for name in ("zig", "zig.exe"):
        bundled = kit_dir / "zig" / name
        if bundled.is_file():
            return zig(bundled)
    if shutil.which("zig"):
        return zig(shutil.which("zig"))
    for cc in ("clang", "gcc", "cc"):
        if shutil.which(cc):
            return [shutil.which(cc)], cc
    fail("no C compiler found: the kit has no zig/ directory and there is no "
         "zig, clang or gcc on PATH")


def compile_flags(kit, kit_dir, gen_dir):
    c = kit["compile"]
    flags = [f"-std={c['std']}", "-O3", "-DNDEBUG", "-fPIC", "-w", "-g0",
             # The lifted code is x86 arithmetic written in C: signed
             # overflow wraps, and memory is read through any pointer type.
             "-fwrapv", "-fno-strict-aliasing",
             # The guest registers are thread-locals of the executable; their
             # offsets are fixed, so no __tls_get_addr per access.
             "-ftls-model=initial-exec",
             # Calls inside the module are direct, not through the PLT.
             "-fno-semantic-interposition"]
    flags += ["-D" + d for d in c["defines"]]
    flags += ["-I" + str(kit_dir / d) for d in c["include_dirs"]]
    flags.append("-I" + str(gen_dir))
    return flags


def compile_one(cc, flags, src, obj):
    """Compile unless obj's stamp says this source and these flags made it."""
    key = hashlib.sha256()
    key.update("\0".join(cc + flags).encode())
    key.update(Path(src).read_bytes())
    stamp = obj.with_suffix(".stamp")
    digest = key.hexdigest()
    if obj.exists() and stamp.exists() and stamp.read_text() == digest:
        return src, None, False
    r = subprocess.run(cc + flags + ["-c", str(src), "-o", str(obj)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return src, (r.stderr or r.stdout)[-3000:], True
    stamp.write_text(digest)
    return src, None, True


# ── main ────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--xbe", required=True, help="default.xbe from the player's disc")
    ap.add_argument("--kit", default=str(DEFAULT_KIT), help="the title kit")
    ap.add_argument("--out", help="module file to write (default: beside the "
                                  "kit directory, i.e. beside the executable)")
    ap.add_argument("--work-dir", help="lift and object cache "
                                       "(default: the user cache directory)")
    ap.add_argument("--cc", help="C compiler command (e.g. 'zig cc', clang)")
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--allow-mismatch", action="store_true",
                    help="build even when the lift differs from the kit's "
                         "reference (the overrides may then be wrong)")
    ap.add_argument("--relift", action="store_true",
                    help="lift again even if the cached lift matches")
    args = ap.parse_args()

    started = time.time()
    kit_dir = Path(args.kit).resolve()
    try:
        kit = json.loads((kit_dir / "kit.json").read_text())
        reference = json.loads((kit_dir / "reference.json").read_text())
    except (OSError, ValueError) as e:
        fail(f"{kit_dir} is not a title kit ({e})")
    if kit.get("format") != KIT_FORMAT:
        fail(f"kit format {kit.get('format')} is not supported by this script "
             f"(expects {KIT_FORMAT})")

    xbe = Path(args.xbe).resolve()
    if not xbe.is_file():
        fail(f"no such file: {xbe}")
    say(f"checking {xbe.name}...")
    xbe_sha = sha256_file(xbe)
    if xbe_sha not in kit["xbe_sha256"]:
        fail(f"{xbe} is not the release this build supports "
             f"({kit['game_name']}, SHA-256 {kit['xbe_sha256'][0][:16]}...; "
             f"this file is {xbe_sha[:16]}...)")

    work = Path(args.work_dir).resolve() if args.work_dir else default_work_dir(kit)
    gen_dir = work / "gen"
    obj_dir = work / "obj"
    work.mkdir(parents=True, exist_ok=True)
    obj_dir.mkdir(exist_ok=True)

    def lift_matches():
        if not gen_dir.is_dir():
            return False, None
        have = gen_manifest(gen_dir)
        diff = sorted(n for n in set(have) | set(reference)
                      if have.get(n) != reference.get(n))
        return not diff, diff

    ok, _ = (False, None) if args.relift else lift_matches()
    if ok:
        say("lift: cached, matches the reference")
    else:
        toolkit = kit_dir / "toolkit"
        lift = kit["lift"]
        argv = [sys.executable, str(toolkit / "scripts" / "recompile.py"), str(xbe),
                "--work-dir", str(work / "pipeline"), "--gen-dir", str(gen_dir),
                "--json", str(work / "analysis.json"),
                "--hle-symbols", str(kit_dir / lift["hle_symbols"]),
                "--project", str(kit_dir / lift["project"]),
                "--split", str(lift["split"]), "--game-name", kit["game_name"]]
        for s in lift["seeds"]:
            argv += ["--seeds", str(kit_dir / s)]
        try:
            import capstone
            have_cs = capstone.__version__
        except ImportError:
            fail("the lifter needs the capstone Python module "
                 "(pip install capstone)")
        want_cs = kit.get("made_from", {}).get("capstone")
        if want_cs and have_cs != want_cs:
            say(f"note: capstone {have_cs} here, the reference was lifted with "
                f"{want_cs}; the result is checked below")
        say("lifting the game code (several minutes; log: "
            f"{work / 'lift.log'})...")
        env = dict(os.environ, PYTHONHASHSEED="0")
        with open(work / "lift.log", "w") as log:
            r = subprocess.run(argv, cwd=toolkit, env=env, stdout=log,
                               stderr=subprocess.STDOUT)
        if r.returncode != 0:
            fail(f"the lift failed; see {work / 'lift.log'}")
        ok, diff = lift_matches()
        if not ok:
            msg = (f"the lift differs from the reference in {len(diff)} files "
                   f"(e.g. {', '.join(diff[:3])}); this usually means a "
                   f"different capstone version. See {work / 'lift.log'}")
            if not args.allow_mismatch:
                fail(msg)
            say("warning: " + msg)
        else:
            say("lift: matches the reference")

    cc, cc_name = find_compiler(kit_dir, args.cc)
    flags = compile_flags(kit, kit_dir, gen_dir)
    sources = sorted(gen_dir.glob("*.c")) + [kit_dir / s for s in kit["compile"]["sources"]]
    objs = []
    jobs = []
    say(f"compiling {len(sources)} files with {cc_name}, {args.jobs} at a time...")
    t0 = time.time()
    with cf.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for src in sources:
            obj = obj_dir / (src.stem + ".o")
            objs.append(obj)
            jobs.append(pool.submit(compile_one, cc, flags, src, obj))
        built = 0
        for done, fut in enumerate(cf.as_completed(jobs), 1):
            src, err, compiled = fut.result()
            if err:
                for j in jobs:
                    j.cancel()
                sys.stderr.write(err + "\n")
                fail(f"compiling {Path(src).name} failed")
            built += compiled
            print(f"\r  {done}/{len(sources)}", end="", flush=True)
    print()
    say(f"compiled {built} ({len(sources) - built} up to date) in "
        f"{time.time() - t0:.0f} s")

    out = (Path(args.out).resolve() if args.out
           else kit_dir.parent / (kit["module_name"] + module_suffix()))
    tmp = out.with_name(out.name + ".tmp")
    link = cc + ["-shared", "-o", str(tmp)] + [str(o) for o in objs]
    if sys.platform != "win32":
        # The module's own symbols bind to itself (recomp_lookup exists in the
        # executable too); its imports from the executable stay undefined
        # until it is loaded.
        link += ["-Wl,-Bsymbolic", "-lm"]
    r = subprocess.run(link, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write((r.stderr or r.stdout)[-3000:] + "\n")
        fail("linking the module failed")
    os.replace(tmp, out)
    (work / "built.json").write_text(json.dumps({
        "module": str(out), "xbe_sha256": xbe_sha,
        "reference": kit["reference"]["digest"], "compiler": cc_name,
        "module_abi": kit["module_abi"]}, indent=2) + "\n")
    say(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB) in "
        f"{time.time() - started:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
