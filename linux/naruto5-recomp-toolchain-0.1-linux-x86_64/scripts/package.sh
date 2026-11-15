#!/usr/bin/env bash
# Rebuild the public release assets from this development tree.
#
# Why this exists: the installed build and the development build must be the
# same program. Editing files in dist/github/... by hand makes them drift.
# This copies the code and scripts that make the runtime/recompiler work,
# rebuilds the bundled eerecomp/vurecomp with the release toolchain and
# regenerates the archives/checksums. Game data (games/, generated/, build/,
# states/, ISO, saves) is never copied.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist/github"
TOOL="$DIST/naruto5-recomp-toolchain-0.1"
SRC="$DIST/naruto5-recomp-tool-0.1"
[ -d "$TOOL" ] && [ -d "$SRC" ] || { echo "dist/github packages not found" >&2; exit 1; }

# The bundled packages have a toolchain PATH/LD_LIBRARY_PATH prefix that the
# development port.sh does not need. Keep it when replacing port.sh.
prefix_for() {
    sed -n '/^# Self-contained toolchain, when present/,/^fi$/p' "$1/scripts/port.sh" 2>/dev/null | head -12
}

sync_source() {
    local target="$1" prefix
    prefix="$(prefix_for "$target")"
    for dir in recomp runtime host web cmake; do
        [ -d "$ROOT/$dir" ] || continue
        mkdir -p "$target/$dir"
        rsync -a --delete --delete-excluded --exclude='.directory' "$ROOT/$dir/" "$target/$dir/"
    done
    # menu/ also holds development screenshots and profiling spreadsheets.
    mkdir -p "$target/menu"
    rsync -a --delete --delete-excluded --exclude='*.png' --exclude='*.csv' --exclude='*.jpg' \
        --exclude='capture.sh' --exclude='README.md' "$ROOT/menu/" "$target/menu/"
    rsync -a "$ROOT/CMakeLists.txt" "$target/CMakeLists.txt"
    for script in disc.py windows.py payloads.py pnach.py gt4_core.py serve.py build_web.sh gpu_profile.py test_vu_codegen.py vu_refresh.sh build_win.py port_win.py vu_refresh_win.py; do
        [ -f "$ROOT/scripts/$script" ] && rsync -a "$ROOT/scripts/$script" "$target/scripts/$script"
    done
    cp "$ROOT/scripts/port.sh" "$target/scripts/port.sh"
    if [ -n "$prefix" ]; then
        python3 - "$target/scripts/port.sh" "$prefix" <<'PY'
import sys
path, block = sys.argv[1], sys.argv[2]
s = open(path).read()
marker = 'here="$ROOT/scripts"\n'
if block and block not in s:
    assert marker in s, path
    s = s.replace(marker, marker + block + '\n', 1)
    open(path, 'w').write(s)
PY
    fi
    bash -n "$target/scripts/port.sh"
    find "$target" -name '__pycache__' -type d -prune -exec rm -rf {} +
    find "$target" -name '*.pyc' -delete
    rm -rf "$target/build" "$target/games" "$target/generated" "$target/iso_path.txt" "$target/install.log"
}

echo "== syncing source into $TOOL and $SRC"
sync_source "$TOOL"
sync_source "$SRC"

echo "== rebuilding bundled eerecomp/vurecomp"
if [ -x "$TOOL/toolchain/bin/zig-cxx" ]; then
    ( cd "$TOOL"
      PATH="$PWD/toolchain/bin:$PWD/toolchain/cmake/bin:$PWD/toolchain/python/bin:$PATH" \
      CXX="$PWD/toolchain/bin/zig-cxx" bash -c '
        . ./scripts/cxx.sh
        cmake -S . -B build/tools -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DPS2_WGPU_DIR="$PWD/toolchain/wgpu-native" "${cxx_flags[@]}" >/dev/null
        ninja -C build/tools eerecomp vurecomp >/dev/null
        cp build/tools/eerecomp build/tools/vurecomp toolchain/bin/
        chmod +x toolchain/bin/eerecomp toolchain/bin/vurecomp
      ' )
    rm -rf "$TOOL/build"
else
    echo "toolchain/zig-cxx not found; keeping existing binaries" >&2
fi

echo "== archives"
cd "$DIST"
rm -f naruto5-recomp-tool-0.1.tar.gz naruto5-recomp-tool-0.1.zip \
      naruto5-recomp-toolchain-0.1-linux-x86_64.tar.gz \
      SHA256SUMS-tool-0.1 SHA256SUMS-toolchain-0.1 SHA256SUMS
tar -czf naruto5-recomp-tool-0.1.tar.gz naruto5-recomp-tool-0.1
zip -qr naruto5-recomp-tool-0.1.zip naruto5-recomp-tool-0.1
tar -czf naruto5-recomp-toolchain-0.1-linux-x86_64.tar.gz naruto5-recomp-toolchain-0.1
find naruto5-recomp-tool-0.1 -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS-tool-0.1
sha256sum naruto5-recomp-tool-0.1.tar.gz naruto5-recomp-tool-0.1.zip >> SHA256SUMS-tool-0.1
find naruto5-recomp-toolchain-0.1 -type f -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS-toolchain-0.1
sha256sum naruto5-recomp-toolchain-0.1-linux-x86_64.tar.gz >> SHA256SUMS-toolchain-0.1
sha256sum naruto5-recomp-tool-0.1.tar.gz naruto5-recomp-tool-0.1.zip \
    naruto5-recomp-toolchain-0.1-linux-x86_64.tar.gz > SHA256SUMS
cat SHA256SUMS
