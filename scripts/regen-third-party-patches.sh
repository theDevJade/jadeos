#!/usr/bin/env bash
# Regenerate third_party/patches/<name>/ from the current state of the submodule
# working trees. Run after editing files inside third_party/<name>/.
#
# For each submodule listed in third_party/SUBMODULES.txt:
#   - Diff vs. the pinned commit -> third_party/patches/<name>/0001-jade.patch
#     (file removed if there is no diff).
#   - Copy any untracked, non-ignored files to
#     third_party/patches/<name>/overlay/<relative-path>.
#
# Files already present in overlay/ but no longer untracked are pruned, with
# one exception: paths under <submodule>/<basename(submodule)>/<file> are
# preserved if they look like new sources you've integrated (treat as a hint;
# inspect `git status` before committing).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PINS="$ROOT/third_party/SUBMODULES.txt"
PATCH_ROOT="$ROOT/third_party/patches"

[[ -f "$PINS" ]] || { echo "ERROR: $PINS missing" >&2; exit 1; }

regen_one() {
  local path="$1" sha="$2"
  local name; name="$(basename "$path")"
  local out_dir="$PATCH_ROOT/$name"
  local patch_file="$out_dir/0001-jade.patch"

  if [[ ! -d "$path/.git" && ! -f "$path/.git" ]]; then
    echo "==> $path: not populated; run scripts/vendor-third-party.sh first."
    return 0
  fi

  mkdir -p "$out_dir"

  echo "==> $path: diff vs $sha"
  local tmp
  tmp="$(mktemp)"
  if git -C "$path" diff --binary --no-color "$sha" > "$tmp"; then
    :
  else
    echo "ERROR: git diff failed inside $path; is $sha fetched?" >&2
    rm -f "$tmp"; exit 1
  fi

  if [[ -s "$tmp" ]]; then
    mv "$tmp" "$patch_file"
    echo "    wrote $patch_file ($(wc -l < "$patch_file") lines)"
  else
    rm -f "$tmp" "$patch_file"
    echo "    no diff"
  fi

  local overlay_dir="$out_dir/overlay"
  mkdir -p "$overlay_dir"

  local n=0
  while IFS= read -r -d '' rel; do
    [[ -n "$rel" ]] || continue
    local src="$path/$rel"
    local dst="$overlay_dir/$rel"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
    n=$((n + 1))
  done < <(cd "$path" && git ls-files --others --exclude-standard -z)

  echo "    overlay: $n untracked file(s) copied -> $overlay_dir"
}

while IFS=$'\t ' read -r path url sha; do
  case "$path" in ''|'#'*) continue ;; esac
  [[ -n "${url:-}" && -n "${sha:-}" ]] || continue
  regen_one "$path" "$sha"
done < <(grep -v '^[[:space:]]*$' "$PINS" | grep -v '^[[:space:]]*#')

echo "==> regen done; review with: git status third_party/patches"
