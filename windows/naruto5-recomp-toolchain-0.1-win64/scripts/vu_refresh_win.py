#!/usr/bin/env python3
"""Compile VU1 microprograms recorded by the Windows launcher.

jogar.bat calls this before/after a session. It only rebuilds when the set of
recorded programs changed, so the common case is instant.
"""
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VU = ROOT / "games" / "naruto" / "vu1"
GEN = ROOT / "generated" / "naruto"
NATIVE = ROOT / "build" / "native-naruto"
TOOLS = ROOT / "build" / "tools"
TOOLCHAIN = ROOT / "toolchain"
def windows_binaries():
    return os.name == "nt" or (TOOLS / "vurecomp.exe").is_file()


def suffix():
    return ".exe" if windows_binaries() else ""


def run(cmd, check=True):
    print("$", " ".join(str(c) for c in cmd), flush=True)
    proc = subprocess.run([str(c) for c in cmd])
    if check and proc.returncode != 0:
        raise SystemExit(proc.returncode)
    return proc.returncode


def stamp():
    h = hashlib.sha256()
    for path in sorted(VU.glob("*.bin")):
        h.update(path.name.encode())
        h.update(str(path.stat().st_size).encode())
    return h.hexdigest()


def main():
    if not VU.is_dir() or not GEN.is_dir():
        return 0
    vurecomp = TOOLS / ("vurecomp" + suffix())
    if not vurecomp.is_file():
        return 0
    manifest = VU / ".manifest"
    now = stamp()
    if manifest.is_file() and manifest.read_text().strip() == now:
        return 0
    print("Otimizando VU1...", flush=True)
    if run([vurecomp, VU, GEN], check=False) != 0:
        print("aviso: vurecomp falhou", file=sys.stderr)
        return 0
    build = ROOT / "scripts" / "build_win.py"
    zig = TOOLCHAIN / "zig" / ("zig.exe" if (TOOLCHAIN / "zig" / "zig.exe").is_file() else "zig")
    sdl = TOOLCHAIN / "sdl2"
    wgpu = TOOLCHAIN / "wgpu-native"
    if build.is_file() and zig.is_file():
        run([sys.executable, "-I", build, "--root", ROOT, "--generated", GEN, "--out", NATIVE,
             "--zig", zig, "--sdl2", sdl, "--wgpu", wgpu, "--jobs", os.cpu_count() or 4], check=False)
    manifest.write_text(now + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
