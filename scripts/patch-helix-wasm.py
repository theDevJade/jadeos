#!/usr/bin/env python3
"""
Idempotent Emscripten patches for vendored arduino-libhelix (Helix AAC).
Run from repo root after cloning arduino-libhelix.
"""
from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]


def patch_assembly() -> bool:
    p = ROOT / "third_party/arduino-libhelix/src/libhelix-aac/assembly.h"
    if not p.is_file():
        return False
    s = p.read_text(encoding="utf-8")
    old = (
        "#elif defined(__GNUC__) && (defined(__i386__) || defined(__amd64__) "
        "|| defined(__APPLE__)) || (defined (_SOLARIS) && !defined (__GNUC__) "
        "&& defined(_SOLARISX86))"
    )
    new = (
        "#elif defined(__GNUC__) && (defined(__i386__) || defined(__amd64__) "
        "|| defined(__APPLE__) || defined(__wasm__)) || (defined (_SOLARIS) "
        "&& !defined (__GNUC__) && defined(_SOLARISX86))"
    )
    if new in s:
        return False
    if old not in s:
        print("patch-helix-wasm: assembly.h pattern not found; skip", file=sys.stderr)
        return False
    p.write_text(s.replace(old, new, 1), encoding="utf-8")
    print("patch-helix-wasm: updated assembly.h for __wasm__")
    return True


def patch_aacdec() -> bool:
    p = ROOT / "third_party/arduino-libhelix/src/libhelix-aac/aacdec.h"
    if not p.is_file():
        return False
    s = p.read_text(encoding="utf-8")
    if re.search(r"defined\(__GNUC__\) && defined\(__wasm__\)", s):
        return False
    pattern = re.compile(
        r"(#elif defined\(__GNUC__\) && defined\(__APPLE__\)\s*#\s*)\n"
        r"(#elif defined\(_OPENWAVE_SIMULATOR\))",
        re.MULTILINE,
    )
    m = pattern.search(s)
    if not m:
        print("patch-helix-wasm: aacdec.h pattern not found; skip", file=sys.stderr)
        return False
    insert = (
        m.group(1)
        + "\n#elif defined(__GNUC__) && defined(__wasm__)\n#\n"
        + m.group(2)
    )
    p.write_text(pattern.sub(insert, s, count=1), encoding="utf-8")
    print("patch-helix-wasm: updated aacdec.h for __wasm__")
    return True


def main() -> int:
    patch_assembly()
    patch_aacdec()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
