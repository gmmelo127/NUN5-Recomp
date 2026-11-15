#!/bin/sh
set -e
version=${1:-v29.0.1.1}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/wgpu-native"
mkdir -p "$target"
curl -L -o "$target/wgpu.zip" "https://github.com/gfx-rs/wgpu-native/releases/download/$version/wgpu-$(uname -s | sed "s/Darwin/macos/;s/Linux/linux/")-$(uname -m | sed "s/arm64/aarch64/")-release.zip"
unzip -o -q "$target/wgpu.zip" -d "$target"
rm "$target/wgpu.zip"
echo "wgpu-native $version in $target"
