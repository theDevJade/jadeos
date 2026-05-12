# Jade portfolio (JadeOS)

A toy OS in the browser: tiled windows, a terminal, small apps, optional Freedoom (doomgeneric), and **media.app** for a bundled PNG plus a **local-only** MP4-derived frame sequence and WAV (see `scripts/export-badapple-media.sh`).

## Licenses

- **`LICENSE`**: MIT covers **new code** in this repo (for example `src/`, `web/`, `deploy/`). It does **not** relicense code inside `third_party/`.
- **`THIRD_PARTY_NOTICES.md`**: lists doomgeneric, OPL, stb, fonts, and demo media. Read it before you ship anything.
- **WASM build**: if you link GPL parts, the **built** program may need to follow GPL rules (source offer, etc.). Get legal advice if you need certainty.

## What you need

- **Native test**: Meson, Ninja, a C++20 compiler.
- **Web build**: `emcc` and `em++` on your `PATH`, plus Meson and Ninja.
- **`third_party/`**: commit it, or run `./scripts/vendor-third-party.sh` once (downloads **stb_image.h** only). It **does not** fetch **doomgeneric** (that tree is Jade-specific and must already be in the repo).

## Vendor script (CI or clean clone)

```bash
./scripts/vendor-third-party.sh
```

## Build locally

```bash
# Web (same idea as deploy)
meson setup build_web --cross-file cross/emscripten.ini --wipe
meson compile -C build_web

# Optional native smoke test
meson setup build_native --wipe && meson compile -C build_native
./build_native/src/jadeportfolio
```

**Bad Apple clip (optional):** install **ffmpeg**, place `assets/videos/badapple.mp4` (gitignored), then run `./scripts/export-badapple-media.sh` or just **`./serve.sh`**, which regenerates outputs when `sequence.txt` is missing.

**Serve** (copies `index.html`, JS/WASM, font, `media/`, and optional Freedoom IWAD; see `serve.sh`):

```bash
./serve.sh
# or: SKIP_NATIVE=1 ./serve.sh
```

Use **media.app** for `assets/images/amazingimage.png` and a **generated** clip under `assets/videos/badapple/` (see export script; source MP4 stays local and is not committed).

## Docker

The **`Dockerfile`** runs **`scripts/vendor-third-party.sh`**, optionally **`scripts/export-badapple-media.sh`** when `assets/videos/badapple.mp4` is in the build context, then Meson, then copies the same files as `serve.sh` (including `media/` when those asset files exist).

```bash
docker build -t jadeportfolio .
```

Your build context must include **`third_party/doomgeneric`** with the Jade integration files.

## Fonts and media

- **Hack** (`assets/fonts/Hack-Regular.ttf`) is an open monospace font; see `THIRD_PARTY_NOTICES.md` for license notes.
- Demo **PNG** under `assets/`: you are responsible for rights on anything you distribute.
- **Bad Apple-style clip**: keep `assets/videos/badapple.mp4` only on your machine (gitignored). Run `./scripts/export-badapple-media.sh` (or let `serve.sh` do it) so `www/media/badapple/` contains frames + PCM WAV + `sequence.txt` for preload.

## Repo layout

| Path | Purpose |
|------|---------|
| `src/` | CPU, GPU, OS, apps, WASM bindings |
| `web/index.html` | Page shell, WebGPU or 2D canvas, preload into MEMFS |
| `cross/emscripten.ini` | Meson cross file for Emscripten |
| `third_party/` | doomgeneric, OPL, stb |
| `scripts/vendor-third-party.sh` | Fetch **stb_image.h** into `third_party/stb/` |
| `scripts/export-badapple-media.sh` | MP4 → PNG sequence + 48 kHz WAV + manifests (local; gitignored) |
| `deploy/` | nginx config for the image |
