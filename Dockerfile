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

RUN chmod +x scripts/vendor-third-party.sh \
    && ./scripts/vendor-third-party.sh

RUN chmod +x scripts/export-badapple-media.sh \
    && (test -f assets/videos/badapple.mp4 \
        && BADAPPLE_MP4=assets/videos/badapple.mp4 ./scripts/export-badapple-media.sh || true)

RUN meson setup /build --cross-file=cross/emscripten.ini \
    && meson compile -C /build

# Match serve.sh: site root plus media/ for media.app.
RUN mkdir -p /opt/site/media \
    && cp web/index.html web/favicon.svg /opt/site/ \
    && cp /build/src/jadeportfolio.js /build/src/jadeportfolio.wasm /opt/site/ \
    && cp assets/fonts/Hack-Regular.ttf /opt/site/Hack-Regular.ttf \
    && (test -f assets/images/amazingimage.png \
        && cp assets/images/amazingimage.png /opt/site/media/amazingimage.png || true) \
    && mkdir -p /opt/site/media/badapple \
    && (test -f assets/videos/badapple/sequence.txt \
        && cp -R assets/videos/badapple/* /opt/site/media/badapple/ || true)

FROM nginx:1.27-alpine

COPY deploy/nginx/default.conf /etc/nginx/conf.d/default.conf
COPY --from=build /opt/site/ /usr/share/nginx/html/

EXPOSE 5883

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:5883/ >/dev/null || exit 1
