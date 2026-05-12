# Third-party notices and licenses

This project **bundles or links** several open-source packages.

Paths are from the repo root.

## Summary (not legal advice)

| Component | Location | License (short) | Notes |
|-----------|----------|-----------------|--------|
| **doomgeneric** (from Chocolate Doom lineage) | `third_party/doomgeneric/` (submodule, pinned in `third_party/SUBMODULES.txt`) | **GPL-2.0 or later** | `third_party/doomgeneric/LICENSE`. Freedoom IWAD is a separate download. |
| **OPL** | `third_party/opl/` | **GPL-2.0+** | See headers in `opl*.c`. |
| **stb_image.h** | `third_party/stb/stb_image.h` | **Public domain / MIT** (see stb repo) | [stb](https://github.com/nothings/stb). Single header, no warranty. |
| **Chocolate Doom (OPL music engine)** | `third_party/patches/doomgeneric/overlay/doomgeneric/i_oplmusic.c`, `midifile.c`, `midifile.h` | **GPL-2.0+** | Vendored from [`chocolate-doom`](https://github.com/chocolate-doom/chocolate-doom) `src/` and copied into the doomgeneric tree by the vendor script. Used by the Jade WASM build to render OPL3 music. |

Jade-specific patches and overlay files under `third_party/patches/` inherit
the license of the file they modify (GPL-2.0+ for everything in doomgeneric,
including the OPL music engine and `midifile.c`).

## Fonts and demo media

- **`assets/fonts/Hack-Regular.ttf`**: **[Hack](https://sourcefoundry.org/hack/)** is distributed under the Hack Open Font License (HOFL) and related Bitstream Vera derivative terms; see the upstream Hack repository for full license text.
- **`assets/images/amazingimage.png`**: demo file; you need rights for anything you ship.
- **Bad Apple-style clip**: source MP4, generated PNG frames, `audio.wav`, and `sequence.txt` under `assets/videos/badapple/` are **gitignored** by default. You are responsible for rights on any source video and for complying with **ffmpeg** license terms when you run the export script.
