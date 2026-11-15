#!/usr/bin/env python3
"""Build the native Windows runner without CMake or a Unix shell.

The release toolchain ships Zig as its C++ compiler. This script compiles the
runtime, the SDL2 window host and the generated game units with
`zig c++ -target x86_64-windows-gnu`, links ps2run.exe and copies the SDL2,
SDL3 and wgpu-native DLLs next to it.

  build_win.py --root <repo> --generated <dir> --out <dir> --zig <zig.exe>
               --sdl2 <sdl2 package> --wgpu <wgpu-native package>
               [--jobs N] [--clean]

It is incremental: an object is rebuilt only when its source or the compiler
flags change, so a runtime fix does not recompile the whole game.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

RUNTIME_SOURCES = [
    "runtime/src/memory.cpp", "runtime/src/dispatch.cpp", "runtime/src/system.cpp",
    "runtime/src/kernel.cpp", "runtime/src/hw.cpp", "runtime/src/cop.cpp",
    "runtime/src/disc.cpp", "runtime/src/dmac.cpp", "runtime/src/vif.cpp",
    "runtime/src/vu_micro.cpp", "runtime/src/vu_jit.cpp", "runtime/src/vu_jit_web.cpp",
    "runtime/src/ipu/ipu.cpp", "runtime/src/gs/gs.cpp", "runtime/src/gs/gif.cpp",
    "runtime/src/gsdump.cpp", "runtime/src/gs/queue.cpp", "runtime/src/gs/raster.cpp",
    "runtime/src/iop/cpu.cpp", "runtime/src/iop/kernel.cpp", "runtime/src/iop/libs.cpp",
    "runtime/src/iop/hw.cpp", "runtime/src/iop/spu2.cpp", "runtime/src/iop/sio2.cpp",
    "runtime/src/iop/io.cpp", "runtime/src/iop/cdvd.cpp", "runtime/src/iop/sif.cpp",
    "runtime/src/iop/cdvdfsv.cpp", "runtime/src/menu/recomp_menu.cpp",
    "runtime/src/menu/game_settings.cpp", "runtime/src/menu/menu_overlay.cpp",
    "runtime/src/gs/gpu/gpu.cpp", "host/main.cpp", "host/window.cpp",
]

SYSTEM_LIBS = ["user32", "gdi32", "ole32", "shell32", "ws2_32", "dxgi", "d3d12",
               "bcrypt", "ntdll", "psapi"]


def embed(root: Path, source: str, name: str, out: Path) -> None:
    text = (root / source).read_text()
    out.write_text('extern const char %s[];\nconst char %s[] = R"WGSL(%s)WGSL";\n' % (name, name, text))


def compile_one(job):
    zig, flags, src, obj, log = job
    obj.parent.mkdir(parents=True, exist_ok=True)
    cmd = [zig, "c++", "-target", "x86_64-windows-gnu", *flags, "-c", str(src), "-o", str(obj)]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        return str(src), proc.stdout
    return str(src), None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--generated", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--zig", required=True)
    parser.add_argument("--sdl2", required=True)
    parser.add_argument("--wgpu", required=True)
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    generated = Path(args.generated).resolve()
    out = Path(args.out).resolve()
    zig = str(Path(args.zig).resolve())
    sdl = Path(args.sdl2).resolve()
    wgpu = Path(args.wgpu).resolve()
    objdir = out / "obj"
    if args.clean:
        shutil.rmtree(objdir, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    objdir.mkdir(parents=True, exist_ok=True)
    embed(root, "runtime/src/gs/gpu/gs.wgsl", "kGsShader", out / "gs_wgsl.cpp")
    embed(root, "runtime/src/gs/gpu/blit.wgsl", "kBlitShader", out / "blit_wgsl.cpp")

    sources = [root / s for s in RUNTIME_SOURCES]
    sources += [out / "gs_wgsl.cpp", out / "blit_wgsl.cpp"]
    # vurecomp.cpp is the tool source, not the game runtime; CMake only
    # links it lazily, but an explicit object list would duplicate main().
    sources += [p for p in sorted(generated.glob("*.cpp")) if p.name != "vurecomp.cpp"]
    flags = [
        "-std=c++20", "-O3", "-march=haswell", "-DNDEBUG", "-DPS2_GPU", "-DPS2_WINDOW", "-DSDL_MAIN_HANDLED",
        "-I", str(root / "runtime/include"), "-I", str(root / "runtime/src"),
        "-I", str(root / "menu"), "-I", str(root / "runtime/src/menu"),
        "-I", str(root / "runtime/src/gs/gpu"),
        "-I", str(generated), "-I", str(sdl / "include"), "-I", str(wgpu / "include"),
        "-Wall", "-Wextra", "-Wno-unused-label", "-Wno-unused-variable",
        "-Wno-unused-parameter", "-Wno-unused-function",
    ]
    digest = hashlib.sha256((" ".join(flags) + "|" + zig).encode()).hexdigest()[:16]
    jobs = []
    manifest = {}
    for src in sources:
        if not src.is_file():
            print("missing source", src, file=sys.stderr)
            return 1
        key = hashlib.sha256(str(src).encode()).hexdigest()[:16]
        obj = objdir / (key + ".o")
        stamp = obj.with_suffix(".stamp")
        sig = "%s|%d|%d|%s" % (digest, src.stat().st_mtime_ns, src.stat().st_size, src)
        old = stamp.read_text() if stamp.is_file() else ""
        manifest[str(src)] = sig
        if old != sig or not obj.is_file():
            jobs.append((zig, flags, src, obj, stamp))

    print("building %d/%d objects with %d jobs" % (len(jobs), len(sources), args.jobs))
    failures = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(compile_one, job): job for job in jobs}
        for future in concurrent.futures.as_completed(futures):
            src, error = future.result()
            done += 1
            if error:
                failures.append((src, error))
                print("FAILED", src, file=sys.stderr)
            elif done % 50 == 0 or done == len(jobs):
                print("  %d/%d" % (done, len(jobs)), flush=True)
    if failures:
        for src, error in failures[:5]:
            print("\n--- %s ---\n%s" % (src, error[-4000:]), file=sys.stderr)
        print("%d object(s) failed" % len(failures), file=sys.stderr)
        return 1
    for job in jobs:
        job[4].write_text(manifest[str(job[2])])

    objects = []
    for src in sources:
        key = hashlib.sha256(str(src).encode()).hexdigest()[:16]
        objects.append(objdir / (key + ".o"))
    rsp = out / "objects.rsp"
    rsp.write_text("\n".join('"%s"' % str(o) for o in objects) + "\n")
    exe = out / "ps2run.exe"
    link = [zig, "c++", "-target", "x86_64-windows-gnu", "-o", str(exe), "@" + str(rsp),
            str(sdl / "lib/x64/SDL2.lib"), str(wgpu / "lib/libwgpu_native.dll.a")]
    for lib in SYSTEM_LIBS:
        link += ["-l" + lib]
    print("linking", exe.name)
    proc = subprocess.run(link, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        print(proc.stdout[-8000:], file=sys.stderr)
        return 1
    for name, source in (("SDL2.dll", sdl / "lib/x64/SDL2.dll"),
                         ("SDL3.dll", sdl / "lib/x64/SDL3.dll"),
                         ("wgpu_native.dll", wgpu / "lib/wgpu_native.dll")):
        if source.is_file():
            shutil.copy2(source, out / name)
    print("built", exe)
    return 0


if __name__ == "__main__":
    sys.exit(main())
