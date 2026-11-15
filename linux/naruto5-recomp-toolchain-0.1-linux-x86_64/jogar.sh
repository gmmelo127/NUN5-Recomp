#!/bin/sh
# Naruto Shippuden: Ultimate Ninja 5 - launcher Linux
cd "$(dirname "$0")" || exit 1
ROOT=$PWD

BIN=build/native-naruto/ps2run
[ -x "$BIN" ] || { echo "Build ausente. Rode ./instalar.sh primeiro."; exit 1; }

ELF=games/naruto/patched/SLES_556.05
[ -f "$ELF" ] || ELF=games/naruto/disc/SLES_556.05
[ -f "$ELF" ] || { echo "Executavel do jogo ausente. Rode ./instalar.sh."; exit 1; }

DISC="${1:-games/naruto/disc.iso}"
[ -f "$DISC" ] || { echo "ISO nao encontrada: $DISC"; echo "Uso: $0 [/caminho/Naruto5.iso]"; exit 1; }

LD_LIBRARY_PATH="$ROOT/toolchain/sdl2/lib:$ROOT/toolchain/lib:${LD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH

NTSC=""
[ "$ELF" = "games/naruto/patched/SLES_556.05" ] && NTSC="--ntsc"

PATCHES=""
[ -f games/naruto/patches.txt ] && PATCHES="--patches games/naruto/patches.txt"

# Compila qualquer VU1 gravado na sessao anterior que ainda nao entrou no binario.
if [ -x scripts/vu_refresh.sh ]; then scripts/vu_refresh.sh || true; fi
if [ -x build/tools/vurecomp ]; then
    PS2_VU_RECORD="$ROOT/games/naruto/vu1"
    export PS2_VU_RECORD
fi
PS2_PERF=1
export PS2_PERF

"$BIN" "$ELF" --disc "$DISC" --window --widescreen $NTSC $PATCHES \
    --memcard games/naruto/memcard.ps2 "$@"
rc=$?

if [ -x scripts/vu_refresh.sh ]; then scripts/vu_refresh.sh || true; fi
exit $rc
