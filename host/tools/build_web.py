#!/usr/bin/env python3
"""Builds a THPS game as a web page (WebGPU, Emscripten) with N64Recomp-browser's web support. Shared by THPS1-3: run it
through a game's wrapper, decomp/thps<N>/recomp-host/tools/build_web.py. Linux, with emsdk active, naga and spirv-opt on
PATH (N64Recomp-browser's README, Using it in a recomp), clang, CMake, Ninja, git and Python 3.11+:

    python3 -I tools/build_web.py --rom <dump> --browser <N64Recomp-browser checkout> [--prepare] [--fast-link]
    python3 -I tools/build_web.py --browser <N64Recomp-browser checkout> --patches     step 1 alone: no ROM

The page is <recomp-host>/build/web/<family>_recomp.html, .js, .wasm and .data (the menu's documents and fonts: no ROM
data), with coi_serviceworker.js beside it. Everything else it makes is under <recomp-host>/build/web-work. Steps, each
skipped when what it makes is there and its inputs haven't changed:

1. The runtime (work/runtime): tools/setup_runtime.py's pinned N64ModernRuntime and RT64 with this host's patches, as a
   desktop build has them, then N64Recomp-browser's web support over them, project by project (its
   patches/apply.cmake): RT64 with plume and hlslpp, N64ModernRuntime and its N64Recomp. The files where this host's
   patches and the web's change the same lines take web/patches/<project>.patch in place of the web's patch, and the
   web's later changes to them by apply.cmake --since (web/patches/files.txt says which and why). Made again from
   nothing whenever N64Recomp-browser's commit, this host's patches or web/patches moved, since apply.cmake can't take a
   checkout from one version of its patches to another (work/runtime.stamp).
2. N64Recomp and RSPRecomp, built natively from that N64ModernRuntime's N64Recomp: the page needs the code the
   web-patched N64Recomp writes (the desktop build's is its own pin, tools/build_host.py).
3. The recompiled game and the audio microcode from the dump, as tools/build_host.py makes them (build/gen), redone
   when the dump, the two tools or the game's tables and symbols changed (build/gen/web.stamp).
4. The page, with emcmake (cmake/thps_host.cmake's EMSCRIPTEN parts), at -O3 unless --fast-link. --prepare stops before.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostenv  # noqa: E402

HOST = hostenv.host_dir()
FAMILY = hostenv.family(HOST)
COMMON = hostenv.COMMON
SHARED = hostenv.SHARED
TOOLS = hostenv.TOOLS

# <N64Recomp-browser project> <its checkout in the runtime>
WEB_PATCHES = [
    ("rt64", "lib/rt64"),
    ("plume", "lib/rt64/src/contrib/plume"),
    ("hlslpp", "lib/rt64/src/contrib/hlslpp"),
    ("N64ModernRuntime", "lib/N64ModernRuntime"),
    ("N64Recomp", "lib/N64ModernRuntime/N64Recomp"),
]
# The files where this host's patches and the web's change the same lines, the web's change to them merged by hand
# into web/patches/<project>.patch from the N64Recomp-browser commit files.txt names (its header says why each).
WEB_MERGED = os.path.join(SHARED, "web", "patches")


def merged_files():
    """{project: [files]} and the N64Recomp-browser commit they were merged from, read from web/patches/files.txt."""
    files, since = {}, None
    for line in open(os.path.join(WEB_MERGED, "files.txt")):
        if line.startswith("# merged from N64Recomp-browser "):
            since = line.split()[-1]
        elif line.strip() and not line.startswith("#"):
            name, path = line.split()[:2]
            files.setdefault(name, []).append(path)
    return files, since


def say(msg):
    print(f"\n=== {msg}", flush=True)


def run(args, **kw):
    print("+ " + " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kw)


def hash_files(paths):
    """SHA-1 over the contents of these files and of every file under these directories (build/ left out), in order."""
    h = hashlib.sha1()
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                dirs[:] = sorted(d for d in dirs if d not in ("build", "__pycache__", ".git"))
                for f in sorted(files):
                    fp = os.path.join(root, f)
                    h.update(os.path.relpath(fp, p).encode())
                    h.update(open(fp, "rb").read())
        elif os.path.isfile(p):
            h.update(open(p, "rb").read())
    return h.hexdigest()


def read(path):
    try:
        return open(path).read()
    except OSError:
        return None


def step_runtime(work, browser):
    say("1/4 runtime: this host's pins and patches, then N64Recomp-browser's web support")
    runtime = os.path.join(work, "runtime")
    head = subprocess.run(["git", "-C", browser, "rev-parse", "HEAD"], capture_output=True, text=True, check=True).stdout.strip()
    stamp = head + " " + hash_files([os.path.join(SHARED, "patches"), os.path.join(TOOLS, "setup_runtime.py"),
                                     os.path.join(browser, "patches"), WEB_MERGED]) + " " + repr(WEB_PATCHES)
    stamp_file = os.path.join(work, "runtime.stamp")
    if read(stamp_file) == stamp and os.path.isdir(runtime):
        print("up to date")
        return runtime
    if os.path.lexists(stamp_file):
        os.remove(stamp_file)
    hostenv.rmtree(runtime)
    run([sys.executable, "-I", os.path.join(TOOLS, "setup_runtime.py"), runtime])
    apply = os.path.join(browser, "patches", "apply.cmake")
    merged, since = merged_files()
    for name, checkout in WEB_PATCHES:
        path = os.path.join(runtime, checkout)
        own = merged.get(name, [])
        run(["cmake", "-P", apply, name, path, *own])
        if own:
            run(["git", "-C", path, "apply", "--whitespace=nowarn", os.path.join(WEB_MERGED, f"{name}.patch")])
            run(["cmake", "-P", apply, name, path, "--since", since, *own])
    open(stamp_file, "w").write(stamp)
    return runtime


def step_recomp_tools(work, runtime):
    say("2/4 N64Recomp and RSPRecomp from the web-patched N64Recomp")
    build = os.path.join(work, "n64recomp")
    if not os.path.isfile(os.path.join(build, "build.ninja")):
        run(["cmake", "-S", os.path.join(runtime, "lib", "N64ModernRuntime", "N64Recomp"), "-B", build, "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"])
    run(["cmake", "--build", build, "--target", "N64RecompCLI", "RSPRecomp"])
    return os.path.join(build, "N64Recomp"), os.path.join(build, "RSPRecomp")


def step_generate(work, rom, n64recomp, rsprecomp):
    say("3/4 the recompiled game and the audio microcode")
    gen = os.path.join(HOST, "build", "gen")
    stamp = hash_files([rom, n64recomp, rsprecomp, os.path.join(HOST, "versions"), os.path.join(HOST, "game"),
                        os.path.join(HOST, "tools"), os.path.join(os.path.dirname(HOST), "symbols"),
                        os.path.join(COMMON, "games.toml")] +
                       [os.path.join(COMMON, f) for f in sorted(os.listdir(COMMON)) if f.endswith(".py")] +
                       [os.path.join(TOOLS, f) for f in ("gen.py", "build_host.py")])
    stamp_file = os.path.join(gen, "web.stamp")
    if read(stamp_file) == stamp:
        print("up to date")
        return
    if os.path.lexists(stamp_file):
        os.remove(stamp_file)
    import build_host
    py = build_host.step_venv(work)
    roms = build_host.identify([rom], work)
    if not roms:
        sys.exit(f"{rom}: not a dump this build knows")
    build_host.step_generate(py, n64recomp, roms, rsprecomp)
    open(stamp_file, "w").write(stamp)


def step_page(runtime, browser, build, fast_link):
    say("4/4 the page")
    run(["embuilder", "build", "sdl2-mt", "freetype-legacysjlj"], stdout=subprocess.DEVNULL)
    if not os.path.isfile(os.path.join(build, "build.ninja")):
        naga = shutil.which("naga")
        if not naga:
            sys.exit("naga is not on PATH (cargo install naga-cli)")
        run(["emcmake", "cmake", "-S", HOST, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             f"-DRECOMP_RUNTIME_DIR={runtime}", f"-DTHPS_HOST_ROOT={COMMON}", "-DTHPS_STUB_FUNCS=OFF",
             f"-DN64RECOMP_BROWSER_DIR={browser}", f"-DRT64_NAGA_EXECUTABLE={naga}",
             f"-DPython3_EXECUTABLE={shutil.which('python3') or sys.executable}"])
    run(["cmake", build, f"-DN64RECOMP_BROWSER_FAST_LINK={'ON' if fast_link else 'OFF'}"], stdout=subprocess.DEVNULL)
    run(["cmake", "--build", build, "--target", f"{FAMILY}_recomp"])
    for ext in ("html", "js", "wasm", "data"):
        p = os.path.join(build, f"{FAMILY}_recomp.{ext}")
        print(f"{p}: {os.path.getsize(p)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", help="your dump (.z64/.n64/.v64)")
    ap.add_argument("--browser", required=True, help="an N64Recomp-browser checkout (a game repository's lib/N64Recomp-browser)")
    ap.add_argument("--patches", action="store_true", help="step 1 alone (no ROM)")
    ap.add_argument("--prepare", action="store_true", help="steps 1-3 (no page)")
    ap.add_argument("--fast-link", action="store_true", help="link at -O1, in seconds (not for a page people play)")
    args = ap.parse_args()
    if not args.patches and not args.rom:
        ap.error("pass --rom, or --patches")
    browser = os.path.abspath(args.browser)
    if not os.path.isfile(os.path.join(browser, "patches", "apply.cmake")):
        sys.exit(f"{browser} is not an N64Recomp-browser checkout")
    work = os.path.join(HOST, "build", "web-work")
    os.makedirs(work, exist_ok=True)
    runtime = step_runtime(work, browser)
    if args.patches:
        return
    n64recomp, rsprecomp = step_recomp_tools(work, runtime)
    step_generate(work, os.path.abspath(args.rom), n64recomp, rsprecomp)
    if args.prepare:
        return
    step_page(runtime, browser, os.path.join(HOST, "build", "web"), args.fast_link)


if __name__ == "__main__":
    main()
