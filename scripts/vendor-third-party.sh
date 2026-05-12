#!/usr/bin/env bash
# Populate third_party/ for both git-submodule-aware contexts (dev / CI with .git)
# and bare contexts (Docker COPY without .git). Idempotent.
#
# Steps per submodule (path / url / sha from third_party/SUBMODULES.txt):
#   1. Ensure the working tree exists at the pinned commit.
#   2. Apply patches under third_party/patches/<name>/*.patch (skipped if already applied).
#   3. Copy overlay files from third_party/patches/<name>/overlay/ into the submodule tree.
#
# Also downloads stb_image.h (header-only) into third_party/stb/.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: need '$1' in PATH" >&2
    exit 1
  }
}

need_cmd git
need_cmd curl

PINS="$ROOT/third_party/SUBMODULES.txt"
PATCH_ROOT="$ROOT/third_party/patches"

if [[ ! -f "$PINS" ]]; then
  echo "ERROR: $PINS missing" >&2
  exit 1
fi

# Treat the parent repo as "git-aware" only if .git exists AND .gitmodules describes things.
PARENT_HAS_GIT=0
if [[ -d "$ROOT/.git" || -f "$ROOT/.git" ]]; then
  PARENT_HAS_GIT=1
fi

ensure_submodule() {
  local path="$1" url="$2" sha="$3"
  local marker="$ROOT/build/.jade-vendor/${path//\//__}.sha"
  mkdir -p "$(dirname "$marker")"

  # Coolify (and any host that does `git submodule update --init` before
  # `docker build`) leaves a `.git` *file* gitlink inside each submodule
  # pointing at `../../.git/modules/<path>`. .dockerignore strips the parent
  # `.git/` from the build context, so the gitlink target is missing inside
  # the image and `git -C <path> ...` fails with "not a git repository".
  # Detect that case and treat the working tree as not populated.
  local broken_gitlink=0
  if [[ -e "$path/.git" ]] && ! git -C "$path" rev-parse --git-dir >/dev/null 2>&1; then
    broken_gitlink=1
  fi

  if [[ "$broken_gitlink" -eq 0 ]] \
     && [[ -f "$marker" ]] \
     && [[ "$(cat "$marker" 2>/dev/null || true)" == "$sha" ]] \
     && [[ -d "$path/.git" || -f "$path/.git" ]]; then
    echo "==> $path: already at $sha (skipping fetch)"
    return 0
  fi

  if [[ "$broken_gitlink" -eq 1 ]]; then
    echo "==> $path: broken submodule gitlink (host-side checkout w/o .git/modules); re-cloning"
    rm -rf "$path"
  fi

  if [[ "$PARENT_HAS_GIT" -eq 1 ]] && git -C "$ROOT" submodule status -- "$path" >/dev/null 2>&1; then
    echo "==> $path: git submodule update --init"
    git -C "$ROOT" submodule update --init --recursive -- "$path"
  fi

  if [[ ! -e "$path/.git" ]]; then
    if [[ -d "$path" ]] && [[ -n "$(ls -A "$path" 2>/dev/null || true)" ]]; then
      echo "ERROR: $path is populated but has no .git, refusing to overwrite." >&2
      echo "       Move it aside and re-run, or delete it if it is a stale Docker layer." >&2
      exit 1
    fi
    echo "==> $path: clone $url"
    rm -rf "$path"
    mkdir -p "$(dirname "$path")"
    git clone --quiet --filter=blob:none "$url" "$path"
  fi

  echo "==> $path: checkout $sha"
  git -C "$path" fetch --quiet --depth=1 origin "$sha" 2>/dev/null || \
    git -C "$path" fetch --quiet origin
  git -C "$path" checkout --quiet --detach "$sha"
  printf '%s\n' "$sha" > "$marker"
}

apply_patches() {
  local path="$1" name="$2"
  local patch_dir="$PATCH_ROOT/$name"
  [[ -d "$patch_dir" ]] || return 0

  shopt -s nullglob
  local patches=("$patch_dir"/*.patch)
  shopt -u nullglob
  if [[ ${#patches[@]} -eq 0 ]]; then
    return 0
  fi

  for p in "${patches[@]}"; do
    if git -C "$path" apply --reverse --check "$p" >/dev/null 2>&1; then
      echo "==> $path: patch already applied: $(basename "$p")"
      continue
    fi
    echo "==> $path: apply $(basename "$p")"
    git -C "$path" apply --whitespace=nowarn "$p"
  done
}

copy_overlay() {
  local path="$1" name="$2"
  local overlay_dir="$PATCH_ROOT/$name/overlay"
  [[ -d "$overlay_dir" ]] || return 0

  echo "==> $path: copy overlay from $overlay_dir"
  # -a preserves perms, -L follows symlinks (none expected). Avoid `cp -r` quirks across BSD/GNU.
  (cd "$overlay_dir" && find . -type f -print0) | while IFS= read -r -d '' rel; do
    src="$overlay_dir/${rel#./}"
    dst="$path/${rel#./}"
    mkdir -p "$(dirname "$dst")"
    cp -f "$src" "$dst"
  done
}

while IFS=$'\t ' read -r path url sha; do
  case "$path" in
    ''|'#'*) continue ;;
  esac
  [[ -n "${url:-}" && -n "${sha:-}" ]] || {
    echo "ERROR: malformed line in SUBMODULES.txt: $path $url $sha" >&2
    exit 1
  }
  name="$(basename "$path")"
  ensure_submodule "$path" "$url" "$sha"
  apply_patches    "$path" "$name"
  copy_overlay     "$path" "$name"
done < <(grep -v '^[[:space:]]*$' "$PINS" | grep -v '^[[:space:]]*#')

mkdir -p "$ROOT/third_party/stb"
if [[ ! -f "$ROOT/third_party/stb/stb_image.h" ]]; then
  echo "==> stb_image.h: download"
  curl -fsSL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" \
    -o "$ROOT/third_party/stb/stb_image.h"
else
  echo "==> stb_image.h: present"
fi

if [[ ! -f "$ROOT/third_party/doomgeneric/doomgeneric/doomgeneric.h" ]]; then
  echo "ERROR: third_party/doomgeneric still missing doomgeneric.h after vendor step." >&2
  exit 1
fi

echo "==> Vendor third_party: done"
