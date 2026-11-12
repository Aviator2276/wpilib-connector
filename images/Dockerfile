# syntax=docker/dockerfile:1
#
# WPILib workspace image for Coder: Java robot development plus the web Sim GUI.
#
# Extends codercom/enterprise-base:ubuntu (noble, user `coder` uid 1000) with
#   - JDK 17 and libatomic1     (WPILib 2026; the robot JVM runs headless)
#   - /opt/wpilib-websim        the web sim: built shell, WebAssembly GUI bundle,
#                               daemon, Gradle init script, HAL bridge
#   - a read-only Gradle cache  (WPILib / GradleRIO dependencies, shared by all
#                               users through GRADLE_RO_DEP_CACHE)
#   - the pinned WPILib VS Code extension
#
# There is no X server, VNC, Mesa or window manager in this image: the GUI
# runs in the user's browser.
#
# Build from the repository root. The deployment target is linux/amd64:
#   docker buildx build -f images/Dockerfile --platform linux/amd64 \
#     -t <registry>/wpilib-workspace:2026 --push .
# Nothing here is amd64-specific; an arm64 build works too, except that
# PhotonLib publishes no arm64 desktop natives.

ARG SEASON=2026
ARG EMSDK_VERSION=4.0.15

# ---- WebAssembly GUI bundle (architecture independent; built on the host arch)
FROM --platform=$BUILDPLATFORM emscripten/emsdk:${EMSDK_VERSION} AS emsdk-amd64
FROM --platform=$BUILDPLATFORM emscripten/emsdk:${EMSDK_VERSION}-arm64 AS emsdk-arm64
FROM emsdk-${BUILDARCH} AS wasm
ARG SEASON
COPY wasm/ /src/wasm/
COPY protocol/ /src/protocol/
RUN WEBSIM_CACHE=/cache WEBSIM_DIST=/dist bash /src/wasm/build.sh "${SEASON}"

# ---- web shell (static files; also architecture independent)
FROM --platform=$BUILDPLATFORM oven/bun:1 AS web
WORKDIR /src/web
COPY web/package.json web/bun.lock ./
RUN bun install --frozen-lockfile
COPY web/ ./
RUN bun run build

# ---- HAL bridge (native code: built for the target architecture, on an older
#      base so it loads on any supported workspace image)
FROM ubuntu:22.04 AS bridge
ARG SEASON
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      g++ cmake make curl unzip zip ca-certificates binutils \
    && rm -rf /var/lib/apt/lists/*
COPY LICENSE /src/LICENSE
COPY wasm/seasons/ /src/wasm/seasons/
COPY protocol/ /src/protocol/
COPY bridge/ /src/bridge/
RUN WEBSIM_CACHE=/cache WEBSIM_BRIDGE_DIST=/dist bash /src/bridge/build.sh "${SEASON}"

# ---- the workspace image
FROM codercom/enterprise-base:ubuntu

ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive

USER root

# libatomic1: required by WPILib's JNI libraries (hard failure on arm64 without it).
# python3-venv: the daemon. brotli: one-time compression of the static files.
# iproute2 / dnsutils / netcat: network tooling carried over from the v1 image.
RUN apt-get update && apt-get install -y --no-install-recommends \
      openjdk-17-jdk-headless \
      libatomic1 \
      python3 python3-venv \
      brotli \
      iproute2 dnsutils netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

# --- /opt/wpilib-websim ---------------------------------------------------------
COPY --from=web /src/web/dist/ /opt/wpilib-websim/web/
COPY --from=wasm /dist/ /opt/wpilib-websim/wasm/
COPY --from=bridge /dist/maven/ /opt/wpilib-websim/maven/
COPY images/gradle/wpilib-websim.gradle /opt/wpilib-websim/gradle/wpilib-websim.gradle
COPY LICENSE THIRD-PARTY-NOTICES.md /opt/wpilib-websim/
COPY daemon/ /tmp/websimd-src/

# The daemon in its own virtualenv; then compress what browsers download
# (the daemon serves the .br / .gz siblings when the browser accepts them).
RUN python3 -m venv /opt/wpilib-websim/venv \
    && /opt/wpilib-websim/venv/bin/pip install --no-cache-dir "/tmp/websimd-src[fast]" \
    && rm -rf /tmp/websimd-src \
    && /opt/wpilib-websim/venv/bin/websimd --help > /dev/null \
    && find /opt/wpilib-websim/web /opt/wpilib-websim/wasm -type f \
         \( -name '*.js' -o -name '*.css' -o -name '*.html' -o -name '*.wasm' \
            -o -name '*.json' -o -name '*.svg' \) -size +1k \
         -exec brotli -q 11 -k {} \; -exec gzip -9 -k {} \; \
    && chmod -R a+rX /opt/wpilib-websim

# --- Gradle read-only dependency cache -----------------------------------------
# Build a throwaway GradleRIO project once to resolve every WPILib dependency,
# then expose the populated cache read-only to all users. Gradle resolves from
# GRADLE_RO_DEP_CACHE without copying into the per-user cache.
#
# The web sim init script is applied during this build, so the artifacts it
# adds to every project (the simulation WebSocket server, and the HAL bridge
# from /opt/wpilib-websim/maven) are resolved and verified here too.
#
# `simulateJavaRelease --dry-run` resolves the simulation-time native zips at
# configuration time WITHOUT launching the robot program — actually executing
# the task would start a robot JVM that never exits and wedge the build. The
# offline dry-run is the guard: it fails if any artifact is missing from the
# cache. `simulateExternalJavaRelease` is what "WPILib: Simulate Robot Code"
# runs: it extracts the native libraries and writes the extension list without
# starting the robot, which lets the build check that the bridge is in both.
COPY --chown=coder:coder images/seed/ /opt/wpilib-seed/project/
RUN mkdir -p /opt/wpilib-seed/gradle/init.d \
    && cp /opt/wpilib-websim/gradle/wpilib-websim.gradle /opt/wpilib-seed/gradle/init.d/ \
    && chown -R coder:coder /opt/wpilib-seed \
    && su coder -c 'cd /opt/wpilib-seed/project \
      && export GRADLE_USER_HOME=/opt/wpilib-seed/gradle \
      && for attempt in 1 2 3; do ./gradlew build -x test --console=plain && break; \
           [ "$attempt" = 3 ] && exit 1; sleep 20; done \
      && ./gradlew simulateJavaRelease --dry-run --console=plain \
      && ./gradlew simulateJavaRelease --offline --dry-run --console=plain \
      && ./gradlew simulateExternalJavaRelease --offline --console=plain \
      && grep -q "Web Sim Bridge" build/sim/release_java.json \
      && test -f build/jni/release/libhalsim_webbridge.so' \
    # Drop everything except the dependency cache and the wrapper distribution.
    && rm -rf /opt/wpilib-seed/gradle/daemon /opt/wpilib-seed/gradle/kotlin-profile \
              /opt/wpilib-seed/gradle/workers /opt/wpilib-seed/gradle/native \
              /opt/wpilib-seed/gradle/init.d \
              /opt/wpilib-seed/gradle/caches/journal-1 \
              /opt/wpilib-seed/project/build /opt/wpilib-seed/project/.gradle \
    && chmod -R a+rX /opt/wpilib-seed

# Deps come from the shared RO cache; the per-user ~/.gradle only ever holds the
# wrapper distribution (~130 MB one-time) and build-local state.
ENV GRADLE_RO_DEP_CACHE=/opt/wpilib-seed/gradle/caches

# Memory discipline for 4 GB workspaces. Gradle: capped JVM + a 2-minute daemon
# idle timeout so the ~400 MB daemon frees itself while the user is simming, not
# building. Java language server (redhat.java): defaults to -Xmx1G per instance
# and code-server can run several; cap it via machine-level settings.
# wpilib.skipSelectSimulateExtension: "Simulate Robot Code" uses the extension
# defaults (which the init script sets) instead of showing a picker.
RUN mkdir -p /etc/skel/.gradle \
    && printf 'org.gradle.jvmargs=-Xmx768m -XX:MaxMetaspaceSize=384m\norg.gradle.parallel=false\norg.gradle.daemon.idletimeout=120000\n' \
         > /etc/skel/.gradle/gradle.properties \
    && mkdir -p /etc/skel/.local/share/code-server/Machine \
    && printf '{\n  "java.jdt.ls.vmargs": "-XX:+UseParallelGC -XX:GCTimeRatio=4 -XX:AdaptiveSizePolicyWeight=90 -Dsun.zip.disableMemoryMapping=true -Xmx1G -Xms100m -XX:MaxMetaspaceSize=512m",\n  "java.gradle.buildServer.enabled": "off",\n  "java.import.gradle.java.home": "/usr/lib/jvm/java-17-openjdk-%s",\n  "java.configuration.detectJdksAtStart": false,\n  "wpilib.skipSelectSimulateExtension": true\n}\n' "${TARGETARCH}" \
         > /etc/skel/.local/share/code-server/Machine/settings.json

# WPILib VS Code extension, pinned. Open VSX — where code-server installs
# extensions from — stopped at 2021.3.1, which generates Gradle-6 projects that
# cannot build on JDK 17. WPILib ships current versions only as GitHub-release
# VSIXs; the wpilib-sim module installs this file into code-server at start.
ARG WPILIB_EXT_VERSION=2026.2.1
RUN curl -fsSL --retry 5 -o "/opt/wpilib-websim/vscode-wpilib-${WPILIB_EXT_VERSION}.vsix" \
      "https://github.com/wpilibsuite/vscode-wpilib/releases/download/v${WPILIB_EXT_VERSION}/vscode-wpilib-${WPILIB_EXT_VERSION}.vsix" \
    && chmod a+r "/opt/wpilib-websim/vscode-wpilib-${WPILIB_EXT_VERSION}.vsix"

USER coder
ENV JAVA_HOME=/usr/lib/jvm/java-17-openjdk-${TARGETARCH}
