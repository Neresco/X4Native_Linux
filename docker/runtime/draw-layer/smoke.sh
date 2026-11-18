#!/usr/bin/env bash
set -euo pipefail
[[ ! -e /dev/dri && ! -e /dev/dxg ]]
export DISPLAY=:99 XDG_RUNTIME_DIR=/tmp/x4-layer-runtime LP_NUM_THREADS=4
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
icd=$(find /usr/share/vulkan/icd.d -name '*lvp*.json' -print -quit)
export VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES="$icd"
export VK_LAYER_PATH=/opt/x4-layer VK_INSTANCE_LAYERS=VK_LAYER_LOCAL_X4_draw_suppression
Xvfb :99 -screen 0 640x360x24 -nolisten tcp > /results/xvfb.log 2>&1 &
display_pid=$!
trap 'kill "$display_pid" 2>/dev/null || true' EXIT
for ((i=0; i<50; i++)); do
    [[ -S /tmp/.X11-unix/X99 ]] && break
    sleep 0.1
done
for mode in 0 1; do
    X4_DRAW_SUPPRESS=$mode timeout --kill-after=2s 25s \
        vkcube --c 60 --width 320 --height 240 --present_mode 0 \
        > "/results/vkcube-$mode.log" 2>&1
    grep -q '\[X4DRAW\] device created' "/results/vkcube-$mode.log"
    if [[ "$mode" == 1 ]]; then
        grep -Eq 'dropped_draw_calls=[1-9][0-9]* result=0' "/results/vkcube-$mode.log"
    else
        grep -q 'dropped_draw_calls=0 result=0' "/results/vkcube-$mode.log"
    fi
    cat "/results/vkcube-$mode.log"
done
echo 'PASS: real Vulkan draw/presentation in passthrough and suppression modes.'
