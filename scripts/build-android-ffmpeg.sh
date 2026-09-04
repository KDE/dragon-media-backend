#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
set -euo pipefail

DEPS_ROOT=${DRAGON_ANDROID_DEPS_ROOT:-$HOME/Android/deps}
NDK=${ANDROID_NDK:-}
NDK_HOST=${ANDROID_NDK_HOST:-linux-x86_64}
VERSION=9.0.1
SRC_DIR=""
ABI=""

usage() {
    sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        arm64-v8a|x86_64) ABI=$1 ;;
        --deps-root)  DEPS_ROOT=${2:?--deps-root needs a value}; shift ;;
        --ndk)        NDK=${2:?--ndk needs a value}; shift ;;
        --ndk-host)   NDK_HOST=${2:?--ndk-host needs a value}; shift ;;
        --version)    VERSION=${2:?--version needs a value}; shift ;;
        --src-dir)    SRC_DIR=${2:?--src-dir needs a value}; shift ;;
        -h|--help)    usage ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

[ -n "$ABI" ] || { echo "usage: build-android-ffmpeg.sh <abi> [options] — see --help" >&2; exit 1; }
[ -n "$NDK" ] || { echo "no NDK: pass --ndk or export ANDROID_NDK" >&2; exit 1; }

API=28
PREBUILT="$NDK/toolchains/llvm/prebuilt/$NDK_HOST/bin"
PREFIX="$DEPS_ROOT/ffmpeg/$ABI"
SRC="${SRC_DIR:-$DEPS_ROOT/src/ffmpeg-$VERSION}"
TMP="$DEPS_ROOT/tmp/ffmpeg-$ABI"

case $ABI in
  arm64-v8a) ARCH=aarch64; TRIPLE=aarch64-linux-android;  CPU=armv8-a; EXTRA=() ;;
  x86_64)    ARCH=x86_64;  TRIPLE=x86_64-linux-android;   CPU=x86-64;  EXTRA=(--disable-x86asm) ;;
  *) echo "unknown ABI: $ABI" >&2; exit 1 ;;
esac

STAMP="$PREFIX/.ffmpeg-version"
if [ -f "$PREFIX/lib/pkgconfig/libavformat.pc" ] && \
   [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$VERSION" ]; then
  echo "ffmpeg $VERSION $ABI already built at $PREFIX — skipping"
  exit 0
fi

if [ -f "$PREFIX/lib/pkgconfig/libavformat.pc" ]; then
  echo "## ffmpeg $ABI at $PREFIX is stale (built for a different version) — rebuilding"
fi

mkdir -p "$DEPS_ROOT/src" "$TMP"
if [ ! -d "$SRC" ]; then
  echo "## fetching ffmpeg-$VERSION"
  curl -sL "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz" | tar -xJ -C "$DEPS_ROOT/src"
fi
cd "$SRC"

make distclean >/dev/null 2>&1 || true

export LDFLAGS="-Wl,-z,max-page-size=16384"
export CFLAGS="-fvisibility=hidden"
./configure \
  --prefix="$PREFIX" \
  --enable-cross-compile --target-os=android \
  --arch=$ARCH --cpu=$CPU \
  --cc="$PREBUILT/$TRIPLE$API-clang" \
  --cxx="$PREBUILT/$TRIPLE$API-clang++" \
  --ar="$PREBUILT/llvm-ar" \
  --nm="$PREBUILT/llvm-nm" \
  --ranlib="$PREBUILT/llvm-ranlib" \
  --strip="$PREBUILT/llvm-strip" \
  --disable-shared --enable-static --enable-pic \
  --disable-programs --disable-doc --disable-htmlpages --disable-manpages --disable-podpages --disable-txtpages \
  --disable-avdevice --disable-swscale \
  --enable-jni \
  --disable-debug \
  --disable-autodetect \
  "${EXTRA[@]}"

make -j"$(nproc)"
make install

printf '%s\n' "$VERSION" > "$STAMP"

echo "## ffmpeg $ABI installed to $PREFIX"
ls -la "$PREFIX/lib"
