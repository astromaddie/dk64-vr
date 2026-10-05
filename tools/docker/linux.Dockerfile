# The pinned Linux build environment for the Steam Frame port of DK64 Recompiled, after the
# SM64 VR and F-Zero X ports': Ubuntu 22.04 (glibc 2.35, the release floor), clang-15 (what
# upstream's CI uses, and it also builds the MIPS patches with ld.lld-15), libstdc++ 12
# (GLIBCXX 3.4.30), CMake 3.31.6, SDL2 2.30.11 built from source with x11, and static builds
# of freetype and libcurl so the package does not depend on what the host ships. The image
# builds for the Docker host's architecture, so on an Apple Silicon Mac (Colima) it is
# arm64, the Steam Frame's.
#
#   docker build -t dk64-vr-linux -f tools/docker/linux.Dockerfile tools/docker
#   tools/docker/build-linux.sh
ARG BASE_IMAGE=ubuntu:22.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive \
    CC=clang-15 \
    CXX=clang++-15

ARG CMAKE_VERSION=3.31.6
ARG CMAKE_SHA256_AARCH64=b4cc788d63112b2749b40627e719eb5d3b8ed8f00c36d77189f4019cfe64bc9e
ARG CMAKE_SHA256_X86_64=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
ARG SDL2_VERSION=2.30.11
ARG SDL2_SHA256=8b8d4aef2038533da814965220f88f77d60dfa0f32685f80ead65e501337da7f
ARG FREETYPE_VERSION=2.13.3
ARG FREETYPE_SHA256=5c3a8e78f7b24c20b25b54ee575d6daa40007a5f4eea2845861c3409b3021747
ARG CURL_VERSION=8.11.1
ARG CURL_SHA256=a889ac9dbba3644271bd9d1302b5c22a088893719b72be3487bc3d401e5c4e80
# OpenXR-SDK release-1.1.63 (commit f2448a8797c85814aa892efc1ab8707900fbcc78), for DK64_VR builds
ARG OPENXR_VERSION=1.1.63
ARG OPENXR_SHA256=5b8dac0608f49498fe8e95f562fe6bcf0bc781e561f137348a8c251d06a88b6f

# libssl-dev supplies the static OpenSSL that libcurl links; mesa-vulkan-drivers (lavapipe) and
# xvfb are only for the container smoke run. python3 runs the ROM decompressor.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-12 g++-12 libstdc++-12-dev ninja-build pkg-config \
        clang-15 lld-15 llvm-15 \
        git ca-certificates wget file rsync \
        python3 zlib1g-dev libssl-dev \
        libgl1-mesa-dev libopengl-dev libegl1-mesa-dev libvulkan-dev \
        libasound2-dev libpulse-dev libudev-dev libdbus-1-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxinerama-dev libxss-dev libxkbcommon-dev \
        libwayland-dev wayland-protocols libdrm-dev libgbm-dev \
        xvfb xauth libgl1-mesa-dri mesa-vulkan-drivers vulkan-tools \
    && rm -rf /var/lib/apt/lists/* \
    && git config --global --add safe.directory '*'

RUN set -eux; \
    arch="$(uname -m)"; \
    case "$arch" in aarch64) sha="$CMAKE_SHA256_AARCH64" ;; x86_64) sha="$CMAKE_SHA256_X86_64" ;; esac; \
    wget -q "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    echo "${sha}  cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" | sha256sum -c -; \
    mkdir -p /opt/cmake; \
    tar -xzf "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz" -C /opt/cmake --strip-components=1; \
    rm "cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"
ENV PATH=/opt/cmake/bin:$PATH

# The libraries are built with the same compiler the game uses. SDL2 stays shared (the
# package ships it in lib/ with RUNPATH $ORIGIN/lib); freetype and curl are static, PIC.
RUN set -eux; \
    cd /tmp; \
    wget -q "https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-${SDL2_VERSION}.tar.gz"; \
    echo "${SDL2_SHA256}  SDL2-${SDL2_VERSION}.tar.gz" | sha256sum -c -; \
    tar -xzf "SDL2-${SDL2_VERSION}.tar.gz"; \
    cmake -S "SDL2-${SDL2_VERSION}" -B build-sdl2 -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF; \
    cmake --build build-sdl2 --target install; \
    wget -q -O freetype.tar.gz "https://download.savannah.gnu.org/releases/freetype/freetype-${FREETYPE_VERSION}.tar.gz"; \
    echo "${FREETYPE_SHA256}  freetype.tar.gz" | sha256sum -c -; \
    tar -xzf freetype.tar.gz; \
    cmake -S "freetype-${FREETYPE_VERSION}" -B build-freetype -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DFT_DISABLE_ZLIB=ON -DFT_DISABLE_BZIP2=ON -DFT_DISABLE_PNG=ON \
        -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON; \
    cmake --build build-freetype --target install; \
    wget -q -O curl.tar.gz "https://github.com/curl/curl/releases/download/curl-$(echo ${CURL_VERSION} | tr . _)/curl-${CURL_VERSION}.tar.gz"; \
    echo "${CURL_SHA256}  curl.tar.gz" | sha256sum -c -; \
    tar -xzf curl.tar.gz; \
    cmake -S "curl-${CURL_VERSION}" -B build-curl -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCURL_USE_OPENSSL=ON -DOPENSSL_USE_STATIC_LIBS=ON -DUSE_NGHTTP2=OFF \
        -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
        -DCURL_DISABLE_LDAP=ON -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt -DCURL_CA_PATH=/etc/ssl/certs; \
    cmake --build build-curl --target install; \
    ldconfig; \
    rm -rf /tmp/*


# The OpenXR loader, static, with its headers, in /opt/openxr (as the SM64 VR port builds it).
# DK64_VR builds link /opt/openxr/lib/libopenxr_loader.a, so the package ships no loader library.
RUN set -eux; \
    cd /tmp; \
    wget -q -O openxr.tar.gz "https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${OPENXR_VERSION}.tar.gz"; \
    echo "${OPENXR_SHA256}  openxr.tar.gz" | sha256sum -c -; \
    tar -xzf openxr.tar.gz; \
    cmake -S "OpenXR-SDK-release-${OPENXR_VERSION}" -B build-openxr -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/openxr -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DDYNAMIC_LOADER=OFF -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
        -DBUILD_WITH_SYSTEM_JSONCPP=OFF -DBUILD_WITH_XCB_HEADERS=OFF -DBUILD_WITH_WAYLAND_HEADERS=OFF; \
    cmake --build build-openxr --target install; \
    echo "OpenXR-SDK release-${OPENXR_VERSION} sha256 ${OPENXR_SHA256}" > /opt/openxr/PINNED; \
    rm -rf /tmp/*

WORKDIR /src
