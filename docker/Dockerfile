FROM registry.fedoraproject.org/fedora:44 AS build
RUN test "$(uname -m)" = x86_64 \
    && dnf -y install gcc-c++ cmake make git vulkan-headers python3 luajit && dnf clean all
WORKDIR /source
COPY CMakeLists.txt LICENSE ./
COPY src/ src/
COPY sdk/ sdk/
COPY third_party/ third_party/
COPY examples/x4mp/ examples/x4mp/
COPY examples/x4mp_platform.h examples/
RUN cmake -S . -B /build/framework -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build/framework --parallel 2 \
    && cmake -S examples/x4mp -B /build/host -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build/host --parallel 2 && ctest --test-dir /build/host --output-on-failure
ARG X4MP_RELEASE_REPOSITORY=https://github.com/Neresco/X4Multiplayer_Mod.git
ARG X4MP_RELEASE_REF=2b549931e3f806095c87aa340c94cc7c6d3a20f6
RUN git init /release && git -C /release remote add origin "$X4MP_RELEASE_REPOSITORY" \
    && git -C /release fetch --depth 1 origin "$X4MP_RELEASE_REF" \
    && git -C /release checkout --detach FETCH_HEAD \
    && test "$(git -C /release rev-parse HEAD)" = "$X4MP_RELEASE_REF"
COPY docker/runtime/control.cpp docker/runtime/host_runtime.lua docker/runtime/
COPY docker/runtime/probe/ docker/runtime/probe/
COPY docker/runtime/draw-layer/ docker/runtime/draw-layer/
COPY docker/pack_extensions.py docker/
COPY docker/tests/test_control.cpp docker/tests/test_host_runtime.lua docker/tests/
RUN c++ -std=c++17 -O2 -fPIC -shared -I sdk docker/runtime/control.cpp -o /build/x4_host_runtime.so \
    && c++ -std=c++17 -O2 -I sdk docker/tests/test_control.cpp -o /build/test_host_control \
    && (cd /build && ./test_host_control) \
    && luajit docker/tests/test_host_runtime.lua \
    && cc -std=c11 -O2 -fPIC -shared -pthread -Wl,-z,defs -fvisibility=hidden \
       docker/runtime/draw-layer/draw_suppression.c -o /build/libx4_draw_suppression.so \
    && python3 docker/pack_extensions.py /source /release /payload
COPY docker/runtime/*.py docker/runtime/
COPY docker/tests/*.py docker/tests/
RUN python3 -m unittest discover -s docker/tests -p 'test_*.py'

FROM registry.fedoraproject.org/fedora:44 AS runtime
RUN dnf -y install \
      mesa-vulkan-drivers vulkan-tools xorg-x11-server-Xvfb \
      libX11 libXrandr libXcursor libXi libXinerama libXrender libxcb \
      openal-soft libogg libvorbis libpng libcurl libxml2 fontconfig freetype \
      alsa-lib dbus-libs SDL2 procps-ng python3 ca-certificates tini \
      glibc.i686 libstdc++.i686 libgcc.i686 zlib-ng-compat.i686 \
    && dnf clean all \
    && mkdir -p /state /cache /data/game /game /opt/x4-host/draw-layer \
    && chown -R 1000:100 /state /cache /data /game
COPY --from=build /payload/ /opt/x4-host/extensions/
COPY --from=build /build/libx4_draw_suppression.so /opt/x4-host/draw-layer/
COPY docker/runtime/draw-layer/VkLayer_x4_draw_suppression.json /opt/x4-host/draw-layer/
COPY docker/runtime/*.py /opt/x4-host/
ENV X4_STATE_DIR=/state X4_CACHE_DIR=/cache X4_GAME_DIR=/data/game \
    X4_GAME_SOURCE=steam X4_STEAM_UPDATE=missing X4_GAME_VERSION=900 \
    X4MP_TRANSPORT=tcp X4MP_LEGACY_NET=0 X4MP_STREAMS=0 X4MP_STREAMSHIPS=0 \
    X4MP_FULLSIM=0 X4MP_PORT=7777 X4MP_STREAM_PORT=7778 \
    X4_DRAW_SUPPRESS=1 X4_RENDER_THREADS=2 X4_FRAME_RATE=15 X4_RUN_SECONDS=1800
USER 1000:100
WORKDIR /game
HEALTHCHECK --interval=30s --timeout=5s --start-period=25m --retries=3 \
    CMD ["python3", "/opt/x4-host/health.py"]
ENTRYPOINT ["/usr/bin/tini", "-g", "--", "python3", "/opt/x4-host/run.py"]
CMD ["run"]
