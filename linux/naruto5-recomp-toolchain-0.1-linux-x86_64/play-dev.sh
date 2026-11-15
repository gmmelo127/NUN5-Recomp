#!/bin/sh
cd "$(dirname "$0")"
export LD_LIBRARY_PATH="$PWD/third_party/wgpu-native/lib:${LD_LIBRARY_PATH:-}"
exec ./build/dev-naruto/ps2run games/naruto/patched/SLES_556.05 \
  --disc "$PWD/jogo.iso" \
  --window --widescreen --ntsc \
  --patches widescreen_recomp.txt \
  --memcard games/naruto/memcard.ps2
