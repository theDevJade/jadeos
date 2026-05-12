# Jade portfolio (JadeOS)

A toy OS in the browser: tiled windows, a terminal, small apps, optional Freedoom (doomgeneric), and **media.app** for a bundled PNG plus a **local-only** MP4-derived frame sequence and WAV (see `scripts/export-badapple-media.sh`).

## Build

```bash
./scripts/vendor-third-party.sh       # populate third_party/ + apply Jade patches/overlays
meson setup build_web --cross-file cross/emscripten.ini
meson compile -C build_web
```

`vendor-third-party.sh` reads pinned commits from `third_party/SUBMODULES.txt`, populates each
submodule (via `git submodule update --init` when `.git` is present, plain `git clone` otherwise),
applies any patches under `third_party/patches/<name>/*.patch`, and copies overlay files from
`third_party/patches/<name>/overlay/`. The Dockerfile runs the same script, so it works in Coolify /
any Docker build with no parent `.git`.

## Patches and overlays

Jade-specific changes to upstream third-party code live in `third_party/patches/`, not inside the
submodule trees, so this repo never needs to fork upstream. See
[`third_party/patches/README.md`](third_party/patches/README.md) for the layout. After editing
files inside any `third_party/<name>/` working tree:

```bash
./scripts/regen-third-party-patches.sh   # writes 0001-jade.patch + copies new files into overlay/
git add third_party/patches/<name>
```

## Licenses

- **`LICENSE`**: MIT covers **new code** in this repo (`src/`, `web/`, `deploy/`, `third_party/patches/`).
- **`THIRD_PARTY_NOTICES.md`**: lists doomgeneric (GPLv2), OPL, stb, fonts, demo media.
