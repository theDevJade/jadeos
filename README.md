# JadeOS

My personal portfolio site, built as a fake OS in the browser. It has tiled windows, a terminal, some small apps, and optional Doom (via doomgeneric). There's also a media.app that can show a PNG and play back a Bad Apple frame sequence + audio if you bring your own source video.

## Building

```bash
./scripts/vendor-third-party.sh       # pulls third_party deps and applies my patches
meson setup build_web --cross-file cross/emscripten.ini
meson compile -C build_web
```

`vendor-third-party.sh` pulls pinned commits from `third_party/SUBMODULES.txt`, runs `git submodule update --init` if there's a `.git` dir (or plain `git clone` otherwise), applies patches from `third_party/patches/<name>/`, and drops in any overlay files. The Dockerfile does the same thing, so Docker builds work fine without a parent `.git`.

## Patching third-party code

Any changes I make to upstream libs live in `third_party/patches/` rather than directly in the submodule trees. If you edit something under `third_party/<name>/`, regenerate the patch with:

```bash
./scripts/regen-third-party-patches.sh
git add third_party/patches/<name>
```

## Licenses

- **`LICENSE`**: MIT, covering my own code in `src/`, `web/`, `deploy/`, and `third_party/patches/`.
- **`THIRD_PARTY_NOTICES.md`**: covers doomgeneric (GPLv2), OPL, stb, fonts, and demo media.
