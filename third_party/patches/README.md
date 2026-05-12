# third_party patches and overlays

Jade-specific changes to vendored upstream code live here, **not** inside the submodule trees.
Each submodule directory in `third_party/` is checked out from upstream at a pinned commit
(see `third_party/SUBMODULES.txt`). `scripts/vendor-third-party.sh` applies the patches and
copies the overlay files on every build, so Coolify / Docker / a fresh CI clone all produce
the same result without needing forks of those upstream repos.

## Layout

```
third_party/patches/<name>/
  0001-*.patch                # `git format-patch`-style diffs against the pinned commit
  overlay/<path-inside-submodule>/file
                              # NEW files Jade adds to the submodule tree (copied as-is)
```

Currently:

- `doomgeneric/overlay/doom_sources.list` — list of upstream `doomgeneric/*.c` files Meson
  compiles. Last line is `doomgeneric_jade.c` (the Jade platform-glue overlay).
- `doomgeneric/overlay/doomgeneric/doomgeneric_jade.c` — Jade WASM platform glue
  (calls `jade_doom_present`, accepts keys from `jade_doom_feed_key`).
- Patches for upstream `doomgeneric/i_sound.c`, `i_input.c`, `i_system.c`, etc. **need to be
  reauthored** — they were lost from an unstaged working tree. The build will fail until
  they are restored. See "Reauthor a patch" below.

## Apply (build-time, run automatically by the vendor script)

```bash
./scripts/vendor-third-party.sh
```

Idempotent: an already-applied patch is detected via `git apply --reverse --check` and
skipped. Overlay files are always re-copied (mtime updated; content identical).

## Reauthor / edit a patch

1. Make your edits inside `third_party/<name>/` (the submodule working tree).
2. Run `./scripts/regen-third-party-patches.sh`. This:
   - Writes the cumulative diff (vs. pinned upstream commit) to
     `third_party/patches/<name>/0001-jade.patch`.
   - Copies any untracked files to
     `third_party/patches/<name>/overlay/...` (preserving relative path).
3. `git add third_party/patches/<name>` and commit.

## Add a new upstream submodule

1. `git submodule add <url> third_party/<name>` at the pinned commit you want.
2. Add a matching row in `third_party/SUBMODULES.txt`.
3. Patch / overlay as needed under `third_party/patches/<name>/`.

## Licensing

`doomgeneric` is GPLv2 and `h264bsd` is BSD. Patches and overlays in this directory are
distributed under the same license as the file they modify or extend. See
`THIRD_PARTY_NOTICES.md` in the repo root.
