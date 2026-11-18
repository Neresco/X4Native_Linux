#!/usr/bin/env bash
set -euo pipefail
# Dockerfile.layer-build container: /source is this folder, /out is ignored output.
cc -std=c11 -O2 -fPIC -shared -pthread -Wall -Wextra -Wno-unused-parameter \
    -Wl,-z,defs -fvisibility=hidden /source/draw_suppression.c \
    -o /out/libx4_draw_suppression.so
cp /source/VkLayer_x4_draw_suppression.json /out/
sha256sum /out/libx4_draw_suppression.so
