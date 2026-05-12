#!/usr/bin/env bash
# Build /media/badapple assets from a local MP4: PNG frames + 48 kHz stereo WAV + sequence.txt.
# Intended outputs are gitignored; keep the source MP4 local (see .gitignore).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INPUT="${BADAPPLE_MP4:-$ROOT/assets/videos/badapple.mp4}"
OUT="${BADAPPLE_OUT:-$ROOT/assets/videos/badapple}"
# Max frame width for WASM (smaller PNGs decode much faster). 0 = no cap (full resolution).
BADAPPLE_MAX_WIDTH="${BADAPPLE_MAX_WIDTH:-640}"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: need '$1' in PATH" >&2
    exit 1
  }
}

need_cmd ffmpeg
need_cmd ffprobe
need_cmd python3

if [[ ! -f "$INPUT" ]]; then
  echo "ERROR: input MP4 not found: $INPUT" >&2
  echo "       Place badapple.mp4 there or set BADAPPLE_MP4=/path/to/video.mp4" >&2
  exit 1
fi

mkdir -p "$OUT"
find "$OUT" -maxdepth 1 -name 'frame_*.png' -delete
rm -f "$OUT/audio.wav" "$OUT/sequence.txt" "$OUT/files.txt"

FPS_RAW="$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate \
  -of default=noprint_wrappers=1:nokey=1 "$INPUT")"
if [[ -z "$FPS_RAW" ]]; then
  FPS_RAW="30/1"
fi
FPS_FLOAT="$(python3 -c "import fractions,sys; print(float(fractions.Fraction(sys.argv[1])))" "$FPS_RAW")"

if [[ "$BADAPPLE_MAX_WIDTH" =~ ^[0-9]+$ ]] && [[ "$BADAPPLE_MAX_WIDTH" -gt 0 ]]; then
  SCALE_VF="fps=${FPS_RAW},scale=trunc(iw*sar/2)*2:trunc(ih/2)*2,setsar=1,scale=min(iw\\,${BADAPPLE_MAX_WIDTH}):-2:flags=bicubic"
  echo "==> Export frames (fps=$FPS_RAW -> $FPS_FLOAT, max_width=$BADAPPLE_MAX_WIDTH) -> $OUT"
else
  SCALE_VF="fps=${FPS_RAW},scale=trunc(iw*sar/2)*2:trunc(ih/2)*2,setsar=1"
  echo "==> Export frames (fps=$FPS_RAW -> $FPS_FLOAT, full width) -> $OUT"
fi

ffmpeg -nostdin -hide_banner -loglevel error -stats -y \
  -i "$INPUT" \
  -map 0:v:0 \
  -vf "$SCALE_VF" \
  -start_number 1 \
  "$OUT/frame_%06d.png"

n="$(find "$OUT" -maxdepth 1 -name 'frame_*.png' | wc -l | tr -d ' ')"
if [[ "$n" -lt 1 ]]; then
  echo "ERROR: no frames written under $OUT" >&2
  exit 1
fi

# Audio trimmed to the same nominal duration as the frame sequence (keeps A/V aligned).
AT="$(python3 -c "import sys; n=int(sys.argv[1]); f=float(sys.argv[2]); print(n/f)" "$n" "$FPS_FLOAT")"
echo "==> Export audio (${AT}s) -> $OUT/audio.wav"
ffmpeg -nostdin -hide_banner -loglevel error -y \
  -i "$INPUT" \
  -vn -t "$AT" \
  -ar 48000 -ac 2 -c:a pcm_s16le \
  "$OUT/audio.wav"

{
  printf 'fps %s\n' "$FPS_FLOAT"
  printf '%s\n' "frame_%06d.png"
  printf '%s\n' "$n"
  printf '%s\n' "audio.wav"
} >"$OUT/sequence.txt"

echo "==> Wrote $OUT/sequence.txt ($n frames). Staged for serve.sh / MEMFS preload."
