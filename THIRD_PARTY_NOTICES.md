# Third-party notices

These are the open-source libraries bundled in this project. All paths are from the repo root.

| Component | Location | License |
|-----------|----------|---------|
| **doomgeneric** | `third_party/doomgeneric/` | GPL-2.0+ — see `third_party/doomgeneric/LICENSE`. Freedoom IWAD is a separate download. |
| **OPL** | `third_party/opl/` | GPL-2.0+ — see headers in `opl*.c`. |
| **stb_image.h** | `third_party/stb/stb_image.h` | Public domain / MIT — [stb](https://github.com/nothings/stb). |
| **Chocolate Doom (OPL music engine)** | `third_party/patches/doomgeneric/overlay/doomgeneric/i_oplmusic.c`, `midifile.c`, `midifile.h` | GPL-2.0+ — vendored from [chocolate-doom](https://github.com/chocolate-doom/chocolate-doom). Used for OPL3 music in the WASM build. |

My patches and overlay files under `third_party/patches/` inherit the license of whatever file they modify (GPL-2.0+ for anything in the doomgeneric tree).

## Fonts and media

- **`assets/fonts/Hack-Regular.ttf`**: [Hack](https://sourcefoundry.org/hack/) font, under the Hack Open Font License and Bitstream Vera terms. See the upstream repo for full text.
- **`assets/images/amazingimage.png`**: placeholder, funny meme.
- **Bad Apple clip**: the source MP4, exported PNG frames, `audio.wav`, and `sequence.txt` under `assets/videos/badapple/` are gitignored. You're responsible for rights on any source video and ffmpeg license compliance when running the export script.
