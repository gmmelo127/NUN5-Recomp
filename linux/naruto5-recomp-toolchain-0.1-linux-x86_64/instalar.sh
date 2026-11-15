#!/usr/bin/env bash
# Instala e compila o Naruto 5 Recomp a partir da sua ISO (Linux).
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD

die() { echo; echo "ERRO: $*" >&2; echo; exit 1; }

echo "== dependecias"
missing=()
command -v cmake   >/dev/null 2>&1 || missing+=("cmake")
command -v ninja   >/dev/null 2>&1 || missing+=("ninja")
command -v python3 >/dev/null 2>&1 || missing+=("python3")

cxx=""
probe=$(mktemp -d)
printf '#include <format>\nint main() { return std::format("{}", 1).empty(); }\n' > "$probe/t.cpp"
for c in "${CXX:-}" g++ clang++; do
  [ -n "$c" ] || continue
  command -v "$c" >/dev/null 2>&1 || continue
  if "$c" -std=c++20 "$probe/t.cpp" -o "$probe/t" >/dev/null 2>&1; then cxx=$c; break; fi
done
rm -rf "$probe"
[ -n "$cxx" ] || missing+=("g++ 13+ ou clang++ 14+ (C++20 <format>)")

if [ ${#missing[@]} -gt 0 ]; then
  echo "faltam: ${missing[*]}"
  echo
  echo "Instale com um destes:"
  echo "  Debian/Ubuntu/Mint: sudo apt install g++ cmake ninja-build python3 libvulkan1 mesa-vulkan-drivers"
  echo "  Fedora:             sudo dnf install gcc-c++ cmake ninja-build python3 vulkan-loader mesa-vulkan-drivers"
  echo "  Arch/Manjaro:       sudo pacman -S gcc cmake ninja python vulkan-radeon"
  echo "  openSUSE:           sudo zypper install gcc-c++ cmake ninja python3 libvulkan1 Mesa-libvulkan1"
  exit 1
fi
echo "   compilador: $cxx"
echo "   cmake $(cmake --version | head -1 | awk '{print $3}'), ninja $(ninja --version), python $(python3 -V 2>&1 | awk '{print $2}')"
export CXX="$cxx"

echo "== ISO"
ISO="${1:-}"
if [ -z "$ISO" ]; then
  for candidate in jogo.iso ../jogo.iso ../../jogo.iso; do
    [ -f "$candidate" ] && { ISO=$candidate; break; }
  done
fi
if [ -z "$ISO" ]; then
  printf "Caminho da ISO do Naruto 5: "
  read -r ISO
fi
[ -f "$ISO" ] || die "ISO nao encontrada: $ISO"

echo "== build (pode levar alguns minutos na primeira vez)"
pn=()
for f in patches/widescreen_recomp.txt widescreen_recomp.txt; do
  [ -f "$f" ] && { pn+=(--pnach "$f"); break; }
done
for f in patches/PALtoNTSC_fixed.pnach PALtoNTSC_fixed.pnach; do
  if [ -f "$f" ]; then pn+=(--pnach-section "NTSC Video Mode" --pnach "$f"); break; fi
done
./scripts/port.sh naruto "$ISO" --native-only "${pn[@]}"

chmod +x jogar.sh desinstalar.sh 2>/dev/null || true
[ -x build/native-naruto/ps2run ] || die "a build nao foi gerada (veja o log acima)"

echo
echo "== pronto"
echo "Rode:  ./jogar.sh"
echo "Log:   games/naruto/last_run.log"
