#!/usr/bin/env python3
"""Native Windows build pipeline (no bash, no WSL).

  python port_win.py naruto "C:\\path\\game.iso" [--frames 12000] [--rounds 4]
                       [--pnach patches\\PALtoNTSC_fixed.pnach]
                       [--pnach-section "NTSC Video Mode"]

It drives the same Python helpers as the Linux port.sh (disc/pnach/payloads/
windows), cross-builds ps2run.exe with the bundled Zig and records VU1
microprograms with the same exploration loop. The package is self-contained:
nothing is downloaded.
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def log(text=""):
    print(text, flush=True)


def quote_path(path):
    return str(path)


class Port:
    def __init__(self, args):
        self.args = args
        self.name = args.name
        self.iso = Path(args.iso).expanduser().resolve()
        self.game = ROOT / "games" / self.name
        self.generated = ROOT / "generated" / self.name
        self.native = ROOT / "build" / ("native-" + self.name)
        self.tools = ROOT / "build" / "tools"
        self.toolchain = Path(args.toolchain).resolve() if args.toolchain else ROOT / "toolchain"
        self.python = Path(args.python).resolve() if args.python else Path(sys.executable)
        self.wine = args.wine
        self.windows = os.name == "nt" or args.wine
        self.jobs = args.jobs or (os.cpu_count() or 4)
        self.zig = Path(args.zig).resolve() if args.zig else self.toolchain / "zig" / ("zig.exe" if os.name == "nt" else "zig")
        self.sdl2 = self.toolchain / "sdl2"
        self.wgpu = self.toolchain / "wgpu-native"
        self.elf = None
        self.entries = self.game / "entries.txt"
        self.overlays = []

    def tool(self, name):
        suffix = ".exe" if self.windows else ""
        return self.tools / (name + suffix)

    def command(self, cmd):
        cmd = [str(c) for c in cmd]
        if self.wine and cmd and cmd[0].lower().endswith(".exe"):
            return ["wine", *cmd]
        return cmd

    def run(self, cmd, cwd=None, env=None, check=True, capture=False):
        full = self.command(cmd)
        log("$ " + " ".join(str(c) for c in cmd))
        kwargs = {"cwd": cwd or ROOT, "env": env}
        if capture:
            kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        proc = subprocess.run(full, **kwargs)
        if check and proc.returncode != 0:
            if capture and proc.stdout:
                log(proc.stdout[-4000:])
            raise SystemExit("comando falhou (%d): %s" % (proc.returncode, " ".join(full)))
        return proc

    def py(self, script, *rest, **kwargs):
        return self.run([self.python, "-I", ROOT / "scripts" / script, *rest], **kwargs)

    def prepare(self):
        if not self.iso.is_file():
            raise SystemExit("ISO nao encontrada: %s" % self.iso)
        self.game.mkdir(parents=True, exist_ok=True)
        (self.game / "disc").mkdir(parents=True, exist_ok=True)
        (self.game / "vu1").mkdir(parents=True, exist_ok=True)
        (self.game / "shots").mkdir(parents=True, exist_ok=True)
        self.generated.mkdir(parents=True, exist_ok=True)
        self.tools.mkdir(parents=True, exist_ok=True)
        self.entries.touch(exist_ok=True)
        for name in ("eerecomp", "vurecomp"):
            src = self.toolchain / "bin" / (name + (".exe" if self.windows else ""))
            dst = self.tool(name)
            if src.is_file() and (not dst.is_file() or src.stat().st_mtime > dst.stat().st_mtime):
                shutil.copy2(src, dst)

    def source_iso(self):
        if self.iso.suffix.lower() != ".zip":
            return self.iso
        target = self.game / "disc.iso"
        if not target.is_file():
            with zipfile.ZipFile(self.iso) as archive:
                images = [m for m in archive.infolist() if m.filename.lower().endswith(".iso")]
                if not images:
                    raise SystemExit("nao ha .iso dentro do zip")
                with archive.open(max(images, key=lambda m: m.file_size)) as src, open(target, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 24)
        return target

    def disc(self):
        iso = self.source_iso()
        # For a .zip source this is the extracted games\naruto\disc.iso, so
        # jogar.bat must receive the ISO, not the archive.
        (ROOT / "iso_path.txt").write_text(str(iso) + "\n")
        log("== [1/6] disc")
        self.py("disc.py", "extract", str(iso), str(self.game / "disc"), "SYSTEM.CNF", capture=True)
        cnf = (self.game / "disc" / "SYSTEM.CNF").read_text(errors="replace")
        match = re.search(r"BOOT2\s*=\s*([^\r\n;]+)", cnf, re.I)
        if not match:
            raise SystemExit("SYSTEM.CNF nao tem BOOT2")
        boot = match.group(1).strip().replace("\\", "/")
        program = boot.split("/")[-1]
        self.py("disc.py", "extract", str(iso), str(self.game / "disc"), program, capture=True)
        self.boot = boot
        log("boots %s" % boot)
        return iso, self.game / "disc" / program

    def patch(self, iso, elf):
        log("== [2/6] executable")
        self.elf = elf
        if self.args.pnach:
            patched_dir = self.game / "patched"
            shutil.rmtree(patched_dir, ignore_errors=True)
            patched_dir.mkdir(parents=True, exist_ok=True)
            patched = patched_dir / elf.name
            patches_txt = self.game / "patches.txt"
            cmd = [self.python, "-I", ROOT / "scripts" / "pnach.py", "apply", elf, patched, patches_txt]
            for section in self.args.pnach_section:
                cmd += ["--section", section]
            cmd += [Path(p).resolve() for p in self.args.pnach]
            self.run(cmd, check=True)
            self.elf = patched
        log(str(self.elf.relative_to(ROOT)))

    def payloads(self, iso, elf):
        # SNR2 modules if the boot program embeds one.
        modules = []
        data = elf.read_bytes()
        if b"SNR2" in data:
            self.py("disc.py", "modules", str(iso), str(self.game / "modules"), capture=True)
            modules = ["--modules", str(self.game / "modules")]
        prg = self.game / "prg"
        if not (prg / ".done").is_file():
            listing = self.py("disc.py", "list", str(iso), capture=True).stdout or ""
            if any(line.strip().endswith(("PRG/ADV.BIN", "PRG/BTL.BIN", "PRG/ETC.BIN")) or " PRG/" in line for line in listing.splitlines()):
                self.py("disc.py", "extract", str(iso), str(prg), "PRG/*", capture=True)
            prg.mkdir(parents=True, exist_ok=True)
            (prg / ".done").write_text("ok\n")
        self.overlays = []
        ov = self.game / "overlays"
        ov.mkdir(parents=True, exist_ok=True)
        if (self.toolchain / "bin").is_dir() or True:
            self.py("payloads.py", str(elf), str(prg), str(ov), capture=True)
        if any(ov.glob("*.bin")):
            self.overlays = ["--overlays", str(ov)]
        return modules

    def translate(self, elf, modules):
        cmd = [self.tool("eerecomp"), elf, "--entries", self.entries, *modules, *self.overlays, "--out", self.generated]
        proc = self.run(cmd, capture=True)
        if proc.stdout:
            log(proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else "")

    def build(self, final=False):
        if self.args.skip_build:
            log("build pulado (--skip-build)")
            return
        cmd = [self.python, "-I", ROOT / "scripts" / "build_win.py",
               "--root", ROOT, "--generated", self.generated, "--out", self.native,
               "--zig", self.zig, "--sdl2", self.sdl2, "--wgpu", self.wgpu,
               "--jobs", self.jobs]
        self.run(cmd, check=True)

    def analysis_key(self, elf, iso):
        h = hashlib.sha256()
        h.update(elf.read_bytes())
        st = Path(iso).stat()
        h.update(("%d:%d" % (st.st_size, int(st.st_mtime))).encode())
        for root in (self.game / "overlays", self.game / "vu1"):
            if root.is_dir():
                for path in sorted(root.rglob("*")):
                    if path.is_file():
                        h.update(path.name.encode())
                        h.update(path.read_bytes())
        if self.entries.is_file():
            h.update(self.entries.read_bytes())
        return h.hexdigest()

    def missing_tokens(self, misses):
        if not misses.is_file():
            return []
        known = {t for t in self.entries.read_text().split() if t}
        out = []
        for token in misses.read_text().split():
            name, _, at = token.rpartition(":")
            norm = (name + ":" if name else "") + ("%x" % int(at, 16))
            if norm not in known:
                out.append(norm)
        return sorted(set(out))

    def explore(self, elf, iso, modules):
        log("== [4/6] exploring")
        key = self.analysis_key(elf, iso)
        cache = self.game / ".analysis-key"
        if not self.args.refresh and not self.args.no_explore and cache.is_file() and cache.read_text().strip() == key and (self.native / "ps2run.exe").is_file():
            log("analysis up to date, skipping exploration (use --refresh to force)")
            return
        if self.args.no_explore or self.args.skip_build:
            log("skipped")
            return
        runner = self.native / ("ps2run.exe" if self.windows else "ps2run")

        def vu1_count():
            d = self.game / "vu1"
            return len(list(d.glob("*.bin"))) if d.is_dir() else 0

        for round_no in range(1, self.args.rounds + 1):
            self.build()
            vu_before = vu1_count()
            misses = self.game / "misses.txt"
            ram = self.game / "explore_ram.bin"
            for old in (misses, ram, self.game / "explore.ps2"):
                try:
                    old.unlink()
                except FileNotFoundError:
                    pass
            env = os.environ.copy()
            env["PS2_MISS_CONTINUE"] = "1"
            env["PS2_DUMP"] = str(ram)
            env["PS2_GPU_LOW_POWER"] = "1"
            env["PS2_VU_RECORD"] = str(self.game / "vu1")
            # Vary the script per round: menus (START/CROSS), the attract/demo
            # mode with no input, then a dialog-heavy pass. Each pass records
            # the VU1 microprograms that scene uses, so rounds continue until
            # neither EE entry points nor VU1 programs grow.
            if round_no % 3 == 2:
                press = []
            elif round_no % 3 == 0:
                press = ["--press", "300:start"]
                for frame in range(600, self.args.frames, 240):
                    press += ["--press", "%d:cross" % frame, "--press", "%d:circle" % (frame + 120)]
            else:
                press = ["--press", "300:start"]
                for frame in range(600, self.args.frames, 300):
                    press += ["--press", "%d:cross" % frame]
            cmd = [runner, elf, "--disc", iso, "--misses", misses, "--memcard", self.game / "explore.ps2",
                   "--frames", self.args.frames, *press, "--frames-dir", self.game / "shots", "--frame-every", "300"]
            self.run(cmd, env=env, check=False)
            captured = 0
            if self.overlays and ram.is_file():
                proc = self.py("windows.py", "capture", str(elf), str(ram), str(self.game / "overlays"), str(misses), capture=True)
                captured = sum(1 for line in (proc.stdout or "").splitlines() if line.startswith("captured"))
            reason = ""
            explore_log = self.game / "explore.log"
            # port_win does not redirect stdout into explore.log; keep the last lines in memory unavailable.
            new = self.missing_tokens(misses)
            if new:
                with open(self.entries, "a") as out:
                    out.write("\n".join(new) + "\n")
            new_vu = vu1_count() - vu_before
            log("round %d: %d new entry points, %d window images, %d new VU1 programs" % (round_no, len(new), captured, new_vu))
            if not new and not captured and new_vu <= 0:
                break
            self.translate(elf, modules)
        key = self.analysis_key(elf, iso)
        cache.write_text(key + "\n")

    def finalize(self):
        log("== [5/6] vector unit programs")
        vu = self.game / "vu1"
        # Static pass first: recover every microprogram listed in the manifest
        # from the user's own ELF, so a fresh install is fully AOT without
        # playing. Recording stays as a fallback for anything not found.
        manifest = ROOT / "scripts" / "vu1_manifest.txt"
        if manifest.is_file() and self.elf.is_file():
            proc = self.run([self.tool("vurecomp"), "--scan", self.elf, manifest, vu], capture=True)
            for line in (proc.stdout or "").splitlines():
                if "vuscan" in line:
                    log(line.strip())
        if any(vu.glob("*.bin")):
            self.run([self.tool("vurecomp"), vu, self.generated], capture=True)
        else:
            log("none recorded, the native runner interprets them")
        log("== [6/6] builds")
        self.build(final=True)

    def smoke(self, elf, iso):
        if self.args.skip_build:
            return None
        runner = self.native / ("ps2run.exe" if self.windows else "ps2run")
        cmd = [runner, elf, "--disc", iso, "--memcard", self.game / "memcard.ps2", "--frames", "300", "--ntsc"]
        if (ROOT / "patches" / "widescreen_recomp.txt").is_file():
            cmd += ["--patches", ROOT / "patches" / "widescreen_recomp.txt"]
        if (self.game / "patches.txt").is_file():
            cmd += ["--patches", self.game / "patches.txt"]
        proc = self.run(cmd, check=False, env=os.environ.copy(), capture=True)
        out = proc.stdout or ""
        for line in out.splitlines():
            if "no translated code" in line or "missing code" in line:
                return line.strip()
        log("Teste de boot: 300 frames sem codigo faltante.")
        return None

    def run_all(self):
        self.prepare()
        iso, elf = self.disc()
        self.patch(iso, elf)
        modules = self.payloads(iso, self.elf)
        log("== [3/6] machine code to C++")
        self.translate(self.elf, modules)
        if self.args.pnach:
            # pnach may add segments; the patched ELF is the one to translate.
            pass
        self.explore(self.elf, iso, modules)
        self.finalize()
        missing = self.smoke(self.elf, iso)
        if missing:
            log("AVISO: ainda falta codigo: %s" % missing)
        log("\nJogue com: jogar.bat")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("iso")
    parser.add_argument("--frames", type=int, default=12000)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--no-explore", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--pnach", action="append", default=[])
    parser.add_argument("--pnach-section", action="append", default=[])
    parser.add_argument("--native-only", action="store_true")
    parser.add_argument("--toolchain")
    parser.add_argument("--python")
    parser.add_argument("--zig")
    parser.add_argument("--jobs", type=int)
    parser.add_argument("--wine", action="store_true", help="developer: run Windows exes through wine")
    args = parser.parse_args()
    Port(args).run_all()


if __name__ == "__main__":
    main()
