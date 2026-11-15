#!/usr/bin/env bash
# Reproduz a captura do Practice Settings a partir de master_mode.ps2.
# Saida: /var/tmp/practice_capture/frame_*.ppm
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-/var/tmp/practice_capture}
rm -rf "$out"; mkdir -p "$out"
./build/native-naruto/ps2run games/naruto/patched/SLES_556.05 \
  --disc ./jogo.iso --memcard memcard.ps2 --widescreen --ntsc \
  --patches games/naruto/patches.txt --patches widescreen_recomp.txt \
  --load-state master_mode.ps2 --resolution 720p --filter bilinear \
  --press 15510:start:10 --press 15550:left:10 --press 15600:cross:10 \
  --press 15650:up:10 --press 15700:cross:10 \
  --press 16000:down:10 --press 16100:down:10 --press 16300:cross:10 \
  --press 16450:cross:10 --press 16650:cross:10 --press 16850:cross:10 \
  --press 17050:square:10 --press 17150:cross:10 --press 17350:cross:10 \
  --press 17550:cross:10 --press 17750:cross:10 \
  --press 18100:square:10 \
  --frames 18400 --frame-every 100 --frames-dir "$out"
echo "captured in $out"
