#!/usr/bin/env python3
"""
Build a title's game module from the player's own XBE, using a title kit
(scripts/make_kit.py).

    python3 kit/toolkit/scripts/build_module.py --xbe /path/to/default.xbe

--xbe takes the XBE itself, the folder it is in, or the disc image (.iso:
xiso or a full Redump dump); from an image only default.xbe is read.

The steps:

  1. the XBE's SHA-256 must be one the kit was made for;
  2. the four pipeline stages (scripts/recompile.py) lift it into the work
     directory, with the kit's seeds, XDK symbols and recomp_manual.c;
  3. the generated code must match the kit's reference lift file for file,
     because the title's overrides are written against exactly that code;
  4. the generated code, recomp_manual.c and the ABI stamp are compiled and
     linked into <module_name>.so (.dll on Windows).

The module goes to the per-user module cache, in a directory named after
the kit (module_dir()): that is where the runtime looks when it carries a
kit, and a new kit builds into a new directory rather than over a module
another build of the runtime may still use. Beside it, xbe.id records which
XBE it was built from (xbe_fingerprint()), so the runtime can tell when the
player has switched discs.

--check stops after step 1 (a launcher validating a disc the player chose);
--progress adds lines "@progress <from> <to> <what>" for a launcher's bar:
the build is between <from> and <to> percent until the next such line.

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
import re
import shutil
import struct
import subprocess
import sys
import threading
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


PROGRESS = False


def progress(lo, hi, what):
    if PROGRESS:
        print(f"@progress {lo} {hi} {what}", flush=True)


def stop_with_parent():
    """For a launcher's child (--progress): if the launcher goes away, take
    the lifter and the compilers down too rather than build for nobody. The
    launcher starts this script as its own process group's leader."""
    if sys.platform == "win32" or os.getpgid(0) != os.getpid():
        return
    parent = os.getppid()

    def watch():
        while True:
            time.sleep(1.0)
            if os.getppid() != parent:
                os.killpg(0, 15)

    threading.Thread(target=watch, daemon=True).start()


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


def module_dir(kit):
    """Where the runtime finds a kit's module: <user data>/xboxrecomp/modules/
    <title id>/<first 16 of the kit id>. Keep in step with recomp_module.c."""
    if sys.platform == "win32":
        base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local"))
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share"))
    return (base / "xboxrecomp" / "modules" / (kit["title_id"] or "title")
            / kit["kit_id"][:16])


def xbe_fingerprint(data):
    """"<size> <timedate> <title id> <version>" in hex, from the XBE header and
    certificate: cheap enough for the runtime to compute at every start (it
    has the XBE in memory) and different between releases. Keep in step with
    recomp_module.c."""
    base, timedate, cert = struct.unpack_from("<I12xII", data, 0x104)
    title_id, = struct.unpack_from("<I", data, cert - base + 0x08)
    version, = struct.unpack_from("<I", data, cert - base + 0xAC)
    return f"{len(data):x} {timedate:08x} {title_id:08x} {version:08x}"


def read_disc_xbe(path, work, toolkit):
    """The XBE's bytes and a path to it on disk, from an XBE, a folder holding
    default.xbe, or a disc image (default.xbe copied out into the work dir)."""
    path = Path(path)
    if path.is_dir():
        hits = [p for p in path.iterdir() if p.name.lower() == "default.xbe"]
        if not hits:
            fail(f"{path} has no default.xbe: choose the folder the disc was "
                 f"extracted to, or the disc image itself")
        path = hits[0]
    if not path.is_file():
        fail(f"no such file: {path}")
    with open(path, "rb") as f:
        head = f.read(4)
    if head == b"XBEH":
        return path.read_bytes(), path
    sys.path.insert(0, str(toolkit))
    try:
        from tools.xiso.xdvdfs import Xiso, XisoError
    except ImportError:
        fail("this kit cannot read disc images (no tools/xiso); give the "
             "extracted default.xbe")
    try:
        with Xiso(str(path)) as iso:
            entry = iso.find("default.xbe")
            if entry is None:
                fail(f"{path.name} is an Xbox disc image with no default.xbe")
            work.mkdir(parents=True, exist_ok=True)
            out = work / "default.xbe"
            iso.extract(entry, str(out))
    except XisoError as e:
        fail(f"{path.name} is neither an XBE nor an Xbox disc image ({e})")
    return out.read_bytes(), out


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
    ap.add_argument("--xbe", "--disc", dest="xbe", required=True,
                    help="the player's default.xbe, the folder it is in, or "
                         "the disc image")
    ap.add_argument("--kit", default=str(DEFAULT_KIT), help="the title kit")
    ap.add_argument("--out", help="module file to write (default: the per-user "
                                  "module cache, see module_dir)")
    ap.add_argument("--work-dir", help="lift and object cache "
                                       "(default: the user cache directory)")
    ap.add_argument("--cc", help="C compiler command (e.g. 'zig cc', clang)")
    ap.add_argument("--jobs", "-j", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--allow-mismatch", action="store_true",
                    help="build even when the lift differs from the kit's "
                         "reference (the overrides may then be wrong)")
    ap.add_argument("--relift", action="store_true",
                    help="lift again even if the cached lift matches")
    ap.add_argument("--check", action="store_true",
                    help="only check that the disc is one this kit supports")
    ap.add_argument("--progress", action="store_true",
                    help="print @progress lines for a launcher")
    ap.add_argument("--print-module-path", action="store_true",
                    help="print where the module goes and exit")
    args = ap.parse_args()
    global PROGRESS
    PROGRESS = args.progress
    if PROGRESS:
        stop_with_parent()

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

    if "kit_id" not in kit:
        fail("this kit has no kit_id; make it again with scripts/make_kit.py")
    out = (Path(args.out).resolve() if args.out
           else module_dir(kit) / (kit["module_name"] + module_suffix()))
    if args.print_module_path:
        print(out)
        return 0

    work = Path(args.work_dir).resolve() if args.work_dir else default_work_dir(kit)
    progress(0, 2, "Checking the disc")
    say(f"checking {args.xbe}...")
    xbe_data, xbe = read_disc_xbe(Path(args.xbe).resolve(), work, kit_dir / "toolkit")
    xbe_sha = hashlib.sha256(xbe_data).hexdigest()
    if xbe_sha not in kit["xbe_sha256"]:
        fail(f"this is not the release this build supports ({kit['game_name']}: "
             f"the disc's default.xbe has SHA-256 {xbe_sha[:16]}..., expected "
             f"{kit['xbe_sha256'][0][:16]}...)")
    if args.check:
        say(f"ok {xbe_sha}")
        return 0
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
        # Unbuffered: the stage headers are what the progress follower reads.
        env = dict(os.environ, PYTHONHASHSEED="0", PYTHONUNBUFFERED="1")
        # The lifter's four stages, as shares of the 2..75% the lift gets
        # (measured: disassembly and identification are most of it).
        stages = {"parse": (2, 4, "Reading the game's executable"),
                  "disasm": (4, 40, "Disassembling the game code"),
                  "identify": (40, 52, "Identifying functions"),
                  "lift": (52, 75, "Translating the game code to C")}
        progress(2, 4, stages["parse"][2])
        done = threading.Event()

        def follow():
            seen = set()
            while not done.wait(1.0):
                try:
                    text = (work / "lift.log").read_text(errors="replace")
                except OSError:
                    continue
                for name in re.findall(r"^\s+\d/4\s+(\w+)\s*$", text, re.M):
                    if name in stages and name not in seen:
                        seen.add(name)
                        progress(*stages[name])

        watcher = threading.Thread(target=follow, daemon=True)
        watcher.start()
        with open(work / "lift.log", "w") as log:
            r = subprocess.run(argv, cwd=toolkit, env=env, stdout=log,
                               stderr=subprocess.STDOUT)
        done.set()
        watcher.join()
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
    progress(75, 98, "Compiling")
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
            if PROGRESS:
                lo = 75 + 23 * done // len(sources)
                progress(lo, min(98, lo + 2), f"Compiling ({done} of {len(sources)})")
            else:
                print(f"\r  {done}/{len(sources)}", end="", flush=True)
    if not PROGRESS:
        print()
    say(f"compiled {built} ({len(sources) - built} up to date) in "
        f"{time.time() - t0:.0f} s")

    progress(98, 100, "Linking")
    out.parent.mkdir(parents=True, exist_ok=True)
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
    (out.parent / "xbe.id").write_text(xbe_fingerprint(xbe_data) + "\n")
    (work / "built.json").write_text(json.dumps({
        "module": str(out), "xbe_sha256": xbe_sha,
        "reference": kit["reference"]["digest"], "compiler": cc_name,
        "module_abi": kit["module_abi"]}, indent=2) + "\n")
    progress(100, 100, "Done")
    say(f"wrote {out} ({out.stat().st_size / 1e6:.1f} MB) in "
        f"{time.time() - started:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
