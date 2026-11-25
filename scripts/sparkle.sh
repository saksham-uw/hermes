#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
version=2.10.1
sha=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
dest="build/sparkle/$version"
if [[ ! -x "$dest/bin/sign_update" ]]; then
  mkdir -p build/sparkle
  tmp=$(mktemp -d build/sparkle/tmp.XXXXXX)
  trap 'rm -rf "$tmp"' EXIT
  curl -fLsS --retry 3 "https://github.com/sparkle-project/Sparkle/releases/download/$version/Sparkle-$version.tar.xz" -o "$tmp/archive"
  echo "$sha  $tmp/archive" | shasum -a 256 -c - >&2
  mkdir "$tmp/dist"
  tar -xf "$tmp/archive" -C "$tmp/dist"
  mv "$tmp/dist" "$dest"
fi
printf '%s\n' "$PWD/$dest"
