# syntax=docker/dockerfile:1
# Static WASM site behind Coolify + Traefik (Traefik handles HTTPS; nginx listens on 5883).

FROM emscripten/emsdk:4.0.23 AS build

USER root
RUN apt-get update \
    && apt-get install -y --no-install-recommends meson ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN meson setup /build --cross-file=cross/emscripten.ini \
    && meson compile -C /build

# Same layout as serve.sh: single directory for index.html, bundle, and font.
RUN mkdir -p /opt/site \
    && cp web/index.html /opt/site/ \
    && cp /build/src/jadeportfolio.js /build/src/jadeportfolio.wasm /opt/site/ \
    && cp assets/fonts/Monaco.ttf /opt/site/Monaco.ttf

FROM nginx:1.27-alpine

COPY deploy/nginx/default.conf /etc/nginx/conf.d/default.conf
COPY --from=build /opt/site/ /usr/share/nginx/html/

EXPOSE 5883

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD wget -qO- http://127.0.0.1:5883/ >/dev/null || exit 1
