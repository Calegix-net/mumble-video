#!/bin/bash
# Builds the minimal FFmpeg the client's H.264 screen sharing links (src/mumble/H264Codec.cpp): the H.264
# decoder, swscale for colour conversion, and the hardware H.264 encoders only. No software encoder, no
# demuxers, no programs - a few megabytes instead of a hundred, and nothing the client does not call.
#
# Usage: build-ffmpeg.sh linux|windows <prefix>
#   linux:   VA-API (Intel, AMD) and NVENC encoders.
#   windows: NVENC, AMF and Media Foundation encoders (cross-built with MinGW). Media Foundation covers
#            Intel Quick Sync and any other vendor's hardware transform.
#
# NVENC and AMF need only their headers at build time; the drivers that implement them are loaded at
# runtime, so a machine without that vendor's GPU simply fails to open that encoder.
set -euo pipefail

TARGET="${1:?usage: build-ffmpeg.sh linux|windows <prefix>}"
PREFIX="${2:?usage: build-ffmpeg.sh linux|windows <prefix>}"

FFMPEG_VERSION=n8.1.3
NV_CODEC_HEADERS_VERSION=n13.0.19.0
AMF_VERSION=v1.4.36

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

git -c advice.detachedHead=false clone -q --depth 1 --branch "${FFMPEG_VERSION}" https://github.com/FFmpeg/FFmpeg.git "${WORK}/ffmpeg"
git -c advice.detachedHead=false clone -q --depth 1 --branch "${NV_CODEC_HEADERS_VERSION}" \
    https://github.com/FFmpeg/nv-codec-headers.git "${WORK}/nv-codec-headers"
make -C "${WORK}/nv-codec-headers" PREFIX="${PREFIX}" install >/dev/null

export PKG_CONFIG_PATH="${PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

common=(
    --prefix="${PREFIX}"
    --disable-everything --disable-programs --disable-doc --disable-network --disable-autodetect
    --disable-avdevice --disable-avfilter --disable-avformat --disable-swresample
    --enable-shared --disable-static --enable-pic
    --enable-decoder=h264 --enable-parser=h264
    --enable-swscale
    --enable-ffnvcodec --enable-nvenc --enable-encoder=h264_nvenc
)

cd "${WORK}/ffmpeg"

case "${TARGET}" in
    linux)
        ./configure "${common[@]}" --enable-vaapi --enable-libdrm --enable-encoder=h264_vaapi
        ;;
    windows)
        git -c advice.detachedHead=false clone -q --depth 1 --branch "${AMF_VERSION}" \
            https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "${WORK}/amf"
        mkdir -p "${PREFIX}/include/AMF"
        cp -r "${WORK}/amf/amf/public/include/"* "${PREFIX}/include/AMF/"
        ./configure "${common[@]}" \
            --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- --pkg-config=pkg-config \
            --extra-cflags="-I${PREFIX}/include" \
            --enable-mediafoundation --enable-encoder=h264_mf \
            --enable-amf --enable-encoder=h264_amf \
            --enable-d3d11va
        ;;
    *)
        echo "build-ffmpeg.sh: unknown target ${TARGET}" >&2
        exit 1
        ;;
esac

make -j"$(nproc)"
make install
