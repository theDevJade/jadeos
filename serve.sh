#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_NATIVE="$SCRIPT_DIR/build_native"
BUILD_WEB="$SCRIPT_DIR/build_web"
SERVE_DIR="$BUILD_WEB/www"
PORT="${PORT:-8080}"

stage_freedoom_iwad() {
  local dest="$1"
  local src="${FREEDOOM_IWAD:-}"

  if [[ -n "$src" ]]; then
    if [[ ! -f "$src" ]]; then
      echo "ERROR: FREEDOOM_IWAD is set but not a file: $src" >&2
      exit 1
    fi
    cp "$src" "$dest/freedoom1.wad"
    echo "==> IWAD: copied from FREEDOOM_IWAD -> $dest/freedoom1.wad"
    return 0
  fi

  for cand in "$SCRIPT_DIR/web/freedoom1.wad" "$SCRIPT_DIR/assets/freedoom1.wad"; do
    if [[ -f "$cand" ]]; then
      cp "$cand" "$dest/freedoom1.wad"
      echo "==> IWAD: copied $(basename "$cand") from ${cand%/*}/"
      return 0
    fi
  done

  local zip="${FREEDOOM_ZIP:-$SCRIPT_DIR/assets/freedoom.zip}"
  if [[ -f "$zip" ]]; then
    if ! unzip -l "$zip" 2>/dev/null | grep -q 'freedoom1\.wad'; then
      echo "WARN: $zip has no freedoom1.wad member; skipping IWAD extract." >&2
      return 1
    fi
    unzip -jo "$zip" '*freedoom1.wad' -d "$dest" >/dev/null
    echo "==> IWAD: extracted freedoom1.wad from $(basename "$zip")"
    return 0
  fi

  echo "==> IWAD: none staged (Freedoom window will show instructions)."
  echo "    Set FREEDOOM_IWAD=/path/to/freedoom1.wad or place:"
  echo "      web/freedoom1.wad   OR   assets/freedoom1.wad   OR   assets/freedoom.zip (release zip)"
  return 1
}

if [[ "${SKIP_NATIVE:-0}" != "1" ]]; then
echo "==> Building native (smoke test)..."
meson setup "$BUILD_NATIVE" --wipe
meson compile -C "$BUILD_NATIVE"
echo "==> Running native smoke test..."
"$BUILD_NATIVE/src/jadeportfolio"
fi

echo "==> Configuring WASM build..."
meson setup "$BUILD_WEB" \
  --cross-file "$SCRIPT_DIR/cross/emscripten.ini" \
  --wipe

echo "==> Compiling WASM..."
meson compile -C "$BUILD_WEB"

BADAPPLE_SRC="$SCRIPT_DIR/assets/videos/badapple"
BADAPPLE_MP4="$SCRIPT_DIR/assets/videos/badapple.mp4"
if [[ ! -f "$BADAPPLE_SRC/sequence.txt" ]]; then
  if [[ -f "$BADAPPLE_MP4" ]]; then
    echo "==> Building /media/badapple from local MP4 (gitignored outputs)..."
    chmod +x "$SCRIPT_DIR/scripts/export-badapple-media.sh"
    BADAPPLE_MP4="$BADAPPLE_MP4" "$SCRIPT_DIR/scripts/export-badapple-media.sh"
  else
    echo "WARN: No $BADAPPLE_SRC/sequence.txt and no $BADAPPLE_MP4 - media.app video tab will show instructions until you export." >&2
  fi
fi

echo "==> Staging to $SERVE_DIR..."
mkdir -p "$SERVE_DIR"
cp "$SCRIPT_DIR/web/index.html"               "$SERVE_DIR/"
cp "$SCRIPT_DIR/web/favicon.svg"              "$SERVE_DIR/"
cp "$SCRIPT_DIR/web/styles.css"               "$SERVE_DIR/"
cp "$SCRIPT_DIR/web/main.js"                  "$SERVE_DIR/"
cp "$BUILD_WEB/src/jadeportfolio.js"              "$SERVE_DIR/"
cp "$BUILD_WEB/src/jadeportfolio.wasm"            "$SERVE_DIR/"
cp "$SCRIPT_DIR/assets/fonts/Hack-Regular.ttf" "$SERVE_DIR/Hack-Regular.ttf"
mkdir -p "$SERVE_DIR/media"
cp "$SCRIPT_DIR/assets/images/amazingimage.png" "$SERVE_DIR/media/amazingimage.png"
if [[ -f "$BADAPPLE_SRC/sequence.txt" ]]; then
  rm -rf "$SERVE_DIR/media/badapple"
  mkdir -p "$SERVE_DIR/media/badapple"
  cp -R "$BADAPPLE_SRC/"* "$SERVE_DIR/media/badapple/"
  echo "==> Media: staged $SERVE_DIR/media/badapple/ (PNG sequence + audio.wav)"
else
  mkdir -p "$SERVE_DIR/media/badapple"
  if [[ -f "$BADAPPLE_SRC/sequence.example.txt" ]]; then
    cp "$BADAPPLE_SRC/sequence.example.txt" "$SERVE_DIR/media/badapple/sequence.example.txt"
    cp "$BADAPPLE_SRC/sequence.example.txt" "$SERVE_DIR/media/badapple/sequence.txt"
    echo "==> Media: staged fallback manifest at $SERVE_DIR/media/badapple/sequence.txt (no clip frames present)"
  else
    echo "==> Media: $SERVE_DIR/media/badapple/ left empty (add badapple.mp4 and run scripts/export-badapple-media.sh)"
  fi
fi

stage_freedoom_iwad "$SERVE_DIR" || true

echo "==> Serving at http://localhost:$PORT"
echo "    Press Ctrl-C to stop."
echo "    Skip native build: SKIP_NATIVE=1 ./serve.sh"

if command -v python3 &>/dev/null; then
  python3 -m http.server "$PORT" --directory "$SERVE_DIR"
elif command -v npx &>/dev/null; then
  npx --yes serve -l "$PORT" "$SERVE_DIR"
else
  echo "ERROR: no suitable HTTP server found (need python3 or npx)" >&2
  exit 1
fi
