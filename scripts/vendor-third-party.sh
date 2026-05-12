#!/usr/bin/env bash
# Fetch vendored deps needed for WASM (stb_image only; media uses PNG + WAV at runtime).
# Does NOT replace third_party/doomgeneric (Jade-specific files must exist).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: need '$1' in PATH" >&2
    exit 1
  }
}

need_cmd curl

if [[ ! -f "$ROOT/third_party/doomgeneric/doomgeneric/doomgeneric.h" ]]; then
  echo "ERROR: third_party/doomgeneric is missing (JadeOS doomgeneric tree)." >&2
  echo "       Copy it in or commit it before building. This script does not clone it." >&2
  exit 1
fi

mkdir -p "$ROOT/third_party/stb"

if [[ ! -f "$ROOT/third_party/stb/stb_image.h" ]]; then
  echo "==> Downloading stb_image.h"
  curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" \
    -o "$ROOT/third_party/stb/stb_image.h"
else
  echo "==> stb_image.h: present"
fi

echo "==> Vendor third_party: done"
