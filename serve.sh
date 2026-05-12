#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_NATIVE="$SCRIPT_DIR/build_native"
BUILD_WEB="$SCRIPT_DIR/build_web"
SERVE_DIR="$BUILD_WEB/www"
PORT="${PORT:-8080}"

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

echo "==> Staging to $SERVE_DIR..."
mkdir -p "$SERVE_DIR"
cp "$SCRIPT_DIR/web/index.html"               "$SERVE_DIR/"
cp "$BUILD_WEB/src/jadeportfolio.js"              "$SERVE_DIR/"
cp "$BUILD_WEB/src/jadeportfolio.wasm"            "$SERVE_DIR/"
cp "$SCRIPT_DIR/assets/fonts/Monaco.ttf"      "$SERVE_DIR/Monaco.ttf"

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
