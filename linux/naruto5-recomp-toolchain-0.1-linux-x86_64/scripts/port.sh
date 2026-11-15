#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
here="$ROOT/scripts"

usage() {
  cat <<'EOF'
usage: scripts/port.sh <name> <game.iso | game.zip | game.7z> [options]

Turns a PS2 disc image you own into a browser build in build/web-<name> and a
native one in build/native-<name>. It reads the boot program named in
SYSTEM.CNF, translates its machine code to C++, runs it headless to find code
the static pass missed (adding those entry points and translating again),
compiles the vector unit programs those runs recorded and builds both.

options:
  --frames N         frames per exploring run (default 12000)
  --press F:BUTTON   input for the exploring runs, repeatable; the default
                     presses start at frame 300 and cross every 300 frames
  --rounds N         exploring runs at most (default 8)
  --no-explore       translate and build without running the game
  --refresh          force exploration even if the analysis cache is valid
  --native-only      skip the browser build and the Emscripten SDK it needs
  --pnach FILE       apply a PCSX2 patch file, repeatable; patches that land in
                     the executable are translated with it, the rest are
                     written into memory at boot or every frame
  --pnach-section S  also apply the patches under [S] in those files, the way
                     enabling them in PCSX2 does; lines outside any section
                     always apply
EOF
}

[ $# -ge 2 ] || { usage; exit 2; }
name="$1"; source_path="$2"; shift 2
frames=12000; rounds=8; explore=1; refresh=0; web=1; presses=(); pnach=()
while [ $# -gt 0 ]; do
  case "$1" in
    --frames) frames="$2"; shift ;;
    --press) presses+=(--press "$2"); shift ;;
    --rounds) rounds="$2"; shift ;;
    --no-explore) explore=0 ;;
    --refresh) refresh=1 ;;
    --native-only) web=0 ;;
    --pnach) pnach+=("$(realpath "$2")"); shift ;;
    --pnach-section) pnach=(--section "$2" "${pnach[@]}"); shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
if [ ${#presses[@]} -eq 0 ]; then
  presses=(--press 300:start)
  for ((f = 600; f < frames; f += 300)); do presses+=(--press "$f:cross"); done
fi

game="$ROOT/games/$name"
generated="$ROOT/generated/$name"
native="$ROOT/build/native-$name"
tools="$ROOT/build/tools"
export EMSDK="${EMSDK:-$ROOT/tools/emsdk}"
setup=()
[ "$web" = 1 ] || setup=(--native-only)
tool_refresh=0
[ -x "$ROOT/toolchain/bin/eerecomp" ] && [ "$ROOT/toolchain/bin/eerecomp" -nt "$tools/eerecomp" ] && tool_refresh=1 || true
[ -x "$ROOT/toolchain/bin/vurecomp" ] && [ "$ROOT/toolchain/bin/vurecomp" -nt "$tools/vurecomp" ] && tool_refresh=1 || true
if { [ "$web" = 1 ] && [ ! -f "$EMSDK/emsdk_env.sh" ]; } || [ ! -x "$tools/eerecomp" ] || [ ! -x "$tools/vurecomp" ] || [ "$tool_refresh" = 1 ]; then "$here/setup.sh" "${setup[@]}"; fi
mkdir -p "$game/disc" "$game/vu1" "$game/shots" "$generated"

echo "== [1/6] disc"
iso="$game/disc.iso"
if [ -e "$iso" ]; then
  echo "already in place"
else
  case "${source_path,,}" in
    *.zip)
      python3 -I - "$source_path" "$iso" <<'EOF'
import shutil, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as archive:
    images = [m for m in archive.infolist() if m.filename.lower().endswith('.iso')]
    if not images:
        sys.exit('no .iso inside the archive')
    with archive.open(max(images, key=lambda m: m.file_size)) as src, open(sys.argv[2], 'wb') as dst:
        shutil.copyfileobj(src, dst, 1 << 24)
EOF
      ;;
    *.7z) 7z e -so -r "$source_path" '*.iso' > "$iso" ;;
    *) ln -s "$(realpath "$source_path")" "$iso" ;;
  esac
fi
python3 -I "$here/disc.py" extract "$iso" "$game/disc" SYSTEM.CNF > /dev/null
boot=$(sed -n 's/^BOOT2 *= *//p' "$game/disc/SYSTEM.CNF" | tr -d '\r')
program=${boot#*\\}
program=${program%;*}
[ -n "$program" ] || { echo "SYSTEM.CNF names no BOOT2 program" >&2; exit 1; }
python3 -I "$here/disc.py" extract "$iso" "$game/disc" "$program" > /dev/null
echo "boots $boot"
echo "PCSX2 patch files for it end in _$(python3 -I "$here/pnach.py" crc "$game/disc/$program").pnach"

echo "== [2/6] executable"
elf="$game/disc/$program"
if [ -f "$game/core.elf" ]; then
  elf="$game/core.elf"
elif python3 -I "$here/disc.py" list "$iso" | awk '{print $4}' | grep -qx CORE.GT4; then
  python3 -I "$here/disc.py" extract "$iso" "$game/disc" CORE.GT4 > /dev/null
  python3 -I "$here/gt4_core.py" "$game/disc/CORE.GT4" "$game/core.elf" > /dev/null
  elf="$game/core.elf"
fi
rm -rf "$game/patched" "$game/patches.txt"
if [ ${#pnach[@]} -gt 0 ]; then
  mkdir -p "$game/patched"
  python3 -I "$here/pnach.py" apply "$elf" "$game/patched/$(basename "$elf")" "$game/patches.txt" "${pnach[@]}"
  elf="$game/patched/$(basename "$elf")"
fi
echo "${elf#$ROOT/}"
modules=()
if grep -q SNR2 "$elf"; then
  python3 -I "$here/disc.py" modules "$iso" "$game/modules"
  modules=(--modules "$game/modules")
fi
overlays=()
mkdir -p "$game/overlays" "$game/prg"
if [ ! -f "$game/prg/.done" ]; then
  # Programs the game copies into the empty executable segments. They are
  # ordinary disc files, so translate them right away instead of waiting for
  # exploration to run into each one.
  if python3 -I "$here/disc.py" list "$iso" | awk '{print $4}' | grep -q '^PRG/'; then
    python3 -I "$here/disc.py" extract "$iso" "$game/prg" 'PRG/*' > /dev/null
  fi
  touch "$game/prg/.done"
fi
if [ -f "$here/payloads.py" ]; then
  python3 -I "$here/payloads.py" "$elf" "$game/prg" "$game/overlays" | sed 's/^/  /' || true
fi
if windows=$(python3 -I "$here/windows.py" list "$elf") && [ -n "$windows" ]; then
  echo "code windows the game rewrites: $(echo "$windows" | tr '\n' ' ')"
fi
if compgen -G "$game/overlays/*.bin" > /dev/null; then
  overlays=(--overlays "$game/overlays")
fi

translate() { "$tools/eerecomp" "$elf" --entries "$game/entries.txt" "${modules[@]}" "${overlays[@]}" --out "$generated" | tail -1; }

# Exploration is the slow part. It only depends on the game files and the
# images/entry lists already captured, not on the recompiler/runtime source, so
# a runtime fix rebuilds incrementally instead of re-exploring.
analysis_key() {
  {
    sha256sum "$elf" 2>/dev/null
    stat -c '%s %Y' "$iso" 2>/dev/null
    find "$game/overlays" "$game/vu1" -type f -print0 2>/dev/null | sort -z | xargs -0 sha256sum 2>/dev/null
    sha256sum "$game/entries.txt" 2>/dev/null
  } | sha256sum | cut -d' ' -f1
}

echo "== [3/6] machine code to C++"
touch "$game/entries.txt"
translate

build_native() {
  . "$here/cxx.sh"
  # Always reconfigure: a new runtime source (or a different toolchain path)
  # must change build.ninja, otherwise the link can miss gpu.cpp and fail with
  # undefined ps2::gs::gpu symbols. Self-contained packages keep wgpu/SDL2 in
  # toolchain/, not third_party/.
  extra=()
  [ -f "$ROOT/toolchain/wgpu-native/lib/libwgpu_native.a" ] && extra+=(-DPS2_WGPU_DIR="$ROOT/toolchain/wgpu-native")
  [ -d "$ROOT/toolchain/sdl2/lib/cmake/SDL2" ] && extra+=(-DSDL2_DIR="$ROOT/toolchain/sdl2/lib/cmake/SDL2")
  cmake -S "$ROOT" -B "$native" -G Ninja -DCMAKE_BUILD_TYPE=Release "${extra[@]}" "${cxx_flags[@]}" -DPS2_GENERATED="$generated" > /dev/null
  ninja -C "$native" ps2run > "$native/ninja.log" 2>&1 || { tail -30 "$native/ninja.log"; exit 1; }
}

echo "== [4/6] exploring"
key=$(analysis_key)
if [ "$explore" = 1 ] && [ "$refresh" = 0 ] && [ -f "$game/.analysis-key" ] && \
   [ "$(cat "$game/.analysis-key" 2>/dev/null)" = "$key" ] && [ -x "$native/ps2run" ]; then
  echo "analysis up to date, skipping exploration (use --refresh to force)"
elif [ "$explore" = 1 ]; then
  for ((round = 1; round <= rounds; round++)); do
    build_native
    rm -f "$game/misses.txt" "$game/explore.ps2" "$game/explore_ram.bin"
    PS2_MISS_CONTINUE=1 PS2_DUMP="$game/explore_ram.bin" PS2_GPU_LOW_POWER=1 PS2_VU_RECORD="$game/vu1" "$native/ps2run" "$elf" --disc "$iso" --misses "$game/misses.txt" --memcard "$game/explore.ps2" \
      --frames "$frames" "${presses[@]}" --frames-dir "$game/shots" --frame-every 300 > "$game/explore.log" 2>&1 || true
    captured=0
    if [ ${#overlays[@]} -gt 0 ] && [ -f "$game/explore_ram.bin" ]; then
      captured=$(python3 -I "$here/windows.py" capture "$elf" "$game/explore_ram.bin" "$game/overlays" "$game/misses.txt" | grep -c '^captured' || true)
    fi
    reason=$(sed -n 's/^\[ee\] halted: //p' "$game/explore.log" | head -1)
    added=$(python3 -I - "$game/entries.txt" "$game/misses.txt" <<'EOF'
import os, sys
entries, misses = sys.argv[1], sys.argv[2]
def token(t):
    name, _, at = t.rpartition(':')
    return (name + ':' if name else '') + '%x' % int(at, 16)
known = {token(t) for t in open(entries).read().split()}
found = [token(t) for t in open(misses).read().split()] if os.path.exists(misses) else []
new = sorted({t for t in found if t not in known})
with open(entries, 'a') as out:
    for t in new:
        out.write(t + '\n')
print(len(new))
EOF
)
    echo "round $round: ${reason:-ran to the end}, $added new entry points, $captured new window images"
    [ "$added" -gt 0 ] || [ "$captured" -gt 0 ] || break
    translate
  done
  analysis_key > "$game/.analysis-key"
  echo "frames of the last run are in games/$name/shots, its log in games/$name/explore.log"
else
  echo "skipped"
fi

echo "== [5/6] vector unit programs"
if [ -f "$ROOT/scripts/vu1_manifest.txt" ]; then
  "$tools/vurecomp" --scan "$elf" "$ROOT/scripts/vu1_manifest.txt" "$game/vu1" | tail -1
fi
if compgen -G "$game/vu1/*.bin" > /dev/null; then
  "$tools/vurecomp" "$game/vu1" "$generated" | tail -1
else
  echo "none recorded, the native runner interprets them and the page compiles them on the fly"
fi

echo "== [6/6] builds"
build_native
if [ "$web" = 1 ]; then
  "$here/build_web.sh" "$name" "$EMSDK"
fi
echo
if [ "$web" = 1 ]; then
  printf 'Done. Serve it with:  python3 scripts/serve.py %q %q\n' "build/web-$name" "games/$name/disc.iso"
  echo "and open http://localhost:8620/"
fi
play=$(printf '%q %q --disc %q --window' "build/native-$name/ps2run" "${elf#$ROOT/}" "games/$name/disc.iso")
[ -f "$game/patches.txt" ] && play="$play --patches $(printf '%q' "games/$name/patches.txt")"
echo "Play it natively:  $play"
