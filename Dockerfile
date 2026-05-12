# syntax=docker/dockerfile:1

FROM emscripten/emsdk:4.0.23 AS build

USER root
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       meson \
       ninja-build \
       git \
       curl \
       ca-certificates \
       python3 \
       ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG BADAPPLE_MP4_URL=""
ARG FREEDOOM_IWAD_URL=""
ARG FREEDOOM_ZIP_URL="https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip"

RUN chmod +x scripts/vendor-third-party.sh \
    && ./scripts/vendor-third-party.sh

RUN BADAPPLE_URL="${BADAPPLE_MP4_URL:-https://badapple.mov/badapple.mp4}"; \
    TMP_BADAPPLE="/tmp/badapple-source.bin"; \
    NEED_FETCH=1; \
    if [ -f assets/videos/badapple.mp4 ]; then \
        if ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
            -of default=noprint_wrappers=1:nokey=1 assets/videos/badapple.mp4 >/dev/null 2>&1; then \
            echo "==> Using existing assets/videos/badapple.mp4"; \
            NEED_FETCH=0; \
        else \
            echo "WARN: Existing assets/videos/badapple.mp4 is invalid; re-fetching source." >&2; \
            rm -f assets/videos/badapple.mp4; \
        fi; \
    fi; \
    if [ "$NEED_FETCH" -eq 1 ]; then \
        rm -f "$TMP_BADAPPLE"; \
        echo "==> Fetching badapple source from ${BADAPPLE_URL}"; \
        curl -fL --retry 3 --connect-timeout 20 --max-time 300 \
            -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36" \
            -H "Accept: video/*,*/*;q=0.8" \
            "$BADAPPLE_URL" -o "$TMP_BADAPPLE"; \
        if ! ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
            -of default=noprint_wrappers=1:nokey=1 "$TMP_BADAPPLE" >/dev/null 2>&1; then \
            echo "WARN: First download was not a valid video stream; retrying with '?download=1'." >&2; \
            curl -fL --retry 3 --connect-timeout 20 --max-time 300 \
                -A "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 Safari/537.36" \
                -H "Accept: video/*,*/*;q=0.8" \
                "${BADAPPLE_URL}?download=1" -o "$TMP_BADAPPLE"; \
        fi; \
        ffprobe -v error -select_streams v:0 -show_entries stream=codec_name \
            -of default=noprint_wrappers=1:nokey=1 "$TMP_BADAPPLE" >/dev/null 2>&1 \
            || (echo "ERROR: BADAPPLE_MP4_URL did not resolve to a valid video stream." >&2; \
                echo "       URL tried: ${BADAPPLE_URL}" >&2; \
                echo "       Use a direct downloadable media file URL (mp4/mov/webm)." >&2; \
                exit 1); \
        mv "$TMP_BADAPPLE" assets/videos/badapple.mp4; \
    fi

RUN if [ ! -f web/freedoom1.wad ]; then \
            if [ -n "$FREEDOOM_IWAD_URL" ]; then \
                echo "==> Fetching freedoom1.wad from FREEDOOM_IWAD_URL"; \
                curl -fL "$FREEDOOM_IWAD_URL" -o web/freedoom1.wad; \
            else \
                FREEDOOM_ZIP_FETCH_URL="${FREEDOOM_ZIP_URL:-https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip}"; \
                echo "==> Fetching Freedoom zip from ${FREEDOOM_ZIP_FETCH_URL}"; \
                curl -fL "$FREEDOOM_ZIP_FETCH_URL" -o /tmp/freedoom.zip; \
                python3 -c "import zipfile,sys; z=zipfile.ZipFile('/tmp/freedoom.zip'); n=next((x for x in z.namelist() if x.lower() == 'freedoom1.wad' or x.lower().endswith('/freedoom1.wad')), None); n or sys.exit('ERROR: freedoom1.wad not found in ZIP'); open('web/freedoom1.wad','wb').write(z.read(n))"; \
            fi; \
            test -f web/freedoom1.wad || (echo "ERROR: Failed to stage web/freedoom1.wad" >&2; exit 1); \
        fi

RUN chmod +x scripts/export-badapple-media.sh \
        && if test -f assets/videos/badapple.mp4; then \
                 BADAPPLE_MP4=assets/videos/badapple.mp4 ./scripts/export-badapple-media.sh; \
             else \
                 echo "ERROR: Missing assets/videos/badapple.mp4 and BADAPPLE_MP4_URL not provided." >&2; \
                 echo "       Production build requires generating /media/badapple assets." >&2; \
                 exit 1; \
             fi

RUN meson setup /build --cross-file=cross/emscripten.ini \
    && meson compile -C /build

# Match serve.sh: site root plus media/ for media.app.
RUN mkdir -p /opt/site/media \
    && cp web/index.html web/favicon.svg /opt/site/ \
    && cp /build/src/jadeportfolio.js /build/src/jadeportfolio.wasm /opt/site/ \
    && cp web/freedoom1.wad /opt/site/freedoom1.wad \
    && cp assets/fonts/Hack-Regular.ttf /opt/site/Hack-Regular.ttf \
    && (test -f assets/images/amazingimage.png \
        && cp assets/images/amazingimage.png /opt/site/media/amazingimage.png || true) \
    && mkdir -p /opt/site/media/badapple \
        && test -f assets/videos/badapple/sequence.txt \
        && cp -R assets/videos/badapple/* /opt/site/media/badapple/

FROM nginx:1.27-alpine

COPY deploy/nginx/default.conf /etc/nginx/conf.d/default.conf
COPY --from=build /opt/site/ /usr/share/nginx/html/

EXPOSE 5883

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:5883/ >/dev/null || exit 1
