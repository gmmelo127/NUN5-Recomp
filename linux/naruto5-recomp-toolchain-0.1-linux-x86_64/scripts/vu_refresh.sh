#!/usr/bin/env bash
# Recompile recently recorded VU1 microprograms.
#
# The runner records a VU1 program the first time the game uploads it (see
# PS2_VU_RECORD). Recording happens while the game runs, so the compiled code
# cannot be linked into the executable that is already executing. jogar.sh
# calls this script after the game exits: it translates any new programs and
# rebuilds the runner, so the next launch uses the compiled VU1 path instead
# of the interpreter.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
vu="$ROOT/games/naruto/vu1"
generated="$ROOT/generated/naruto"
native="$ROOT/build/native-naruto"
[ -d "$vu" ] && [ -d "$generated" ] || exit 0
[ -x "$ROOT/build/tools/vurecomp" ] || exit 0

manifest="$vu/.manifest"
stamp() {
    find "$vu" -maxdepth 1 -name '*.bin' -printf '%f %s\n' | sort | sha256sum | cut -d' ' -f1
}
now="$(stamp)"
[ -f "$manifest" ] && [ "$(cat "$manifest" 2>/dev/null)" = "$now" ] && exit 0

if ! "$ROOT/build/tools/vurecomp" "$vu" "$generated" > /tmp/naruto-vu.log 2>&1; then
    echo "aviso: vurecomp falhou; veja /tmp/naruto-vu.log" >&2
    exit 0
fi
if [ -f "$native/build.ninja" ]; then
    ninja_bin="$(command -v ninja || true)"
    [ -x "$ROOT/toolchain/bin/ninja" ] && ninja_bin="$ROOT/toolchain/bin/ninja"
    [ -n "$ninja_bin" ] || exit 0
    if "$ninja_bin" -C "$native" ps2run > /tmp/naruto-vu-build.log 2>&1; then
        echo "VU1 atualizado: $("$ROOT/build/tools/vurecomp" "$vu" "$generated" 2>/dev/null | tail -1)"
    else
        echo "aviso: falha ao rebuildar o runner; veja /tmp/naruto-vu-build.log" >&2
    fi
fi
printf '%s\n' "$now" > "$manifest"
