#!/usr/bin/env bash
# The engine bundle PotentialCrates plays through: yt-dlp, a private Python and ffmpeg/ffprobe, per CPU. It is taken unchanged
# from Crate Digger 2.0.0's release (sd88me/mpc-vst-cratedigger, built there by scripts/build-deps.sh and build-python.sh,
# which this repo keeps), checked against the checksums below, and laid out as the plugin expects:
#   vst/build/engine/<armv7|aarch64>/Content/engine/bin/...      (the plugin finds Content/engine next to its .so)
#   vst/build/engine/<armv7|aarch64>/Content/channels.example.txt
#   vst/build/engine/<armv7|aarch64>/Content/engine/LICENSES/    (GPL-3.0 for ffmpeg, the Unlicense for yt-dlp; Python's is in its lib)
# usage: tools/fetch_engine.sh   (needs curl, unzip, sha256sum)
set -euo pipefail
cd "$(dirname "$0")/.."
BASE=https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.0.0
declare -A SUM=(
  [armv7]=e09e2ee728fed7010b9149bbca5cb020820bb950812629c262e2c04dcca07096
  [aarch64]=9b0a900b4b384452e0414b380b8b63be17765797f6c417b33e6b9d5fc4bdcb08
)
mkdir -p vst/build/engine
for arch in armv7 aarch64; do
  zip=vst/build/engine/cratedigger-$arch.zip
  [ -f "$zip" ] || curl -fsSL -o "$zip" "$BASE/Crate-Digger-2.0.0-mpc-$arch.zip"
  echo "${SUM[$arch]}  $zip" | sha256sum -c -
  out=vst/build/engine/$arch
  rm -rf "$out" vst/build/engine/tmp && mkdir -p "$out" vst/build/engine/tmp
  unzip -q "$zip" 'Crate-Digger-2.0.0/portable/*/Content/*' 'Crate-Digger-2.0.0/MODES' -d vst/build/engine/tmp
  src=$(echo vst/build/engine/tmp/Crate-Digger-2.0.0/portable/*/Content)
  cp -a "$src" "$out/Content"
  # the zip keeps exec bits and links in MODES ("x<TAB>path", "l<TAB>path<TAB>target"), relative to the plugin folder
  while IFS=$'\t' read -r kind path target; do
    case "$path" in Content/*) ;; *) continue ;; esac
    if [ "$kind" = x ]; then chmod +x "$out/$path"
    elif [ "$kind" = l ]; then rm -f "$out/$path"; ln -s "$target" "$out/$path"; fi
  done < vst/build/engine/tmp/Crate-Digger-2.0.0/MODES
  mkdir -p "$out/Content/engine/LICENSES"
  cp licenses/GPL-3.0.txt licenses/yt-dlp-UNLICENSE.txt "$out/Content/engine/LICENSES/"
  cp THIRD_PARTY.md "$out/Content/engine/LICENSES/THIRD_PARTY.md"
  # the search daemon is source (src/core): Crate Digger 2.0.0's zip carries an older copy, so ours replaces it
  cp src/core/yt_dlp_daemon.py "$out/Content/engine/bin/yt_dlp_daemon.py"
  rm -rf vst/build/engine/tmp
  test -x "$out/Content/engine/bin/yt-dlp" && test -x "$out/Content/engine/bin/ffmpeg"
  echo "engine $arch: $(du -sh "$out/Content" | cut -f1)"
done
