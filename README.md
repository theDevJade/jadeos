# Jade portfolio (JadeOS)

A toy OS in the browser: tiled windows, a terminal, small apps, optional Freedoom (doomgeneric), and **media.app** for a bundled PNG plus a **local-only** MP4-derived frame sequence and WAV (see `scripts/export-badapple-media.sh`).

## Licenses

- **`LICENSE`**: MIT covers **new code** in this repo (for example `src/`, `web/`, `deploy/`). It does **not** relicense code inside `third_party/`.
- **`THIRD_PARTY_NOTICES.md`**: lists doomgeneric, OPL, stb, fonts, and demo media. Read it before you ship anything.
- **WASM build**: if you link GPL parts, the **built** program may need to follow GPL rules (source offer, etc.). Get legal advice if you need certainty.

## What you need

- **Native test**: Meson, Ninja, a C++20 compiler.
- **Web build**: `emcc` and `em++` on your `PATH`, plus Meson and Ninja.
- **`third_party/`**: several trees are **git submodules** (see `.gitmodules`). After clone run `git submodule update --init --recursive`. `./scripts/vendor-third-party.sh` downloads **stb_image.h** and will run submodule init automatically **only when `.git` exists** (not inside a plain Docker `COPY` build).

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

**Bad Apple clip (optional):** install **ffmpeg**, place `assets/videos/badapple.mp4` (gitignored), then run `./scripts/export-badapple-media.sh` or just **`./serve.sh`**, which regenerates outputs when `sequence.txt` is missing. For smoother WASM playback, frames are capped to **640px width** by default (`BADAPPLE_MAX_WIDTH`; set `0` for full resolution).

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

Your build context must include **populated `third_party/`** (submodule trees). Either:

1. **CI / Coolify:** enable **submodule checkout** on the repo, *or* run `git submodule update --init --recursive` in a pre-build step so `third_party/*` is on disk before `docker build`, **or**
2. **Vendor locally:** run `git submodule update --init --recursive` then `docker build` from that machine (the context will contain the files; `.dockerignore` does not exclude `third_party/`).

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
