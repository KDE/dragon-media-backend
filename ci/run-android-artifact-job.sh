#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# CI artifact build for Android (android_qt611 job).

if [ ! -d "$CI_PROJECT_DIR/kissfft-src" ]; then
    git clone https://github.com/mborgerding/kissfft.git --depth=1 --branch 131.2.0 "$CI_PROJECT_DIR/kissfft-src"
fi
set -euo pipefail

ABI=arm64-v8a
QT_ANDROID=/home/user/android-arm64-clang
IMAGE_ARM64_PREFIX=/home/user/android-arm64-clang

: "${ANDROID_NDK:?the CI image must export ANDROID_NDK}"
: "${ANDROID_HOME:?the CI image must export ANDROID_HOME}"
: "${JAVA_HOME:?the CI image must export JAVA_HOME}"
: "${CI_PROJECT_DIR:?only for CI use}"

git config --global --add safe.directory "$CI_PROJECT_DIR" 2>/dev/null || true

export QT_HOST_PATH=${QT_HOST_PATH:-/opt/nativetooling}
export QT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR:-$QT_HOST_PATH/lib/cmake}
export ECM_DIR=${ECM_DIR:-$QT_HOST_PATH/share/ECM/cmake}
[ -d "$QT_HOST_PATH" ] || { echo "QT_HOST_PATH does not exist: $QT_HOST_PATH" >&2; exit 1; }
[ -d "$QT_ANDROID/lib/cmake/Qt6" ] || { echo "Qt for Android missing: $QT_ANDROID" >&2; exit 1; }

DEPS_ROOT=${DRAGON_ANDROID_DEPS_ROOT:-$CI_PROJECT_DIR/.ci-android-deps}
export DRAGON_ANDROID_DEPS_ROOT="$DEPS_ROOT"
mkdir -p "$DEPS_ROOT"

bash "$CI_PROJECT_DIR/scripts/build-android-ffmpeg.sh" "$ABI" \
    --deps-root "$DEPS_ROOT" \
    --ndk "$ANDROID_NDK" \
    --ndk-host "${ANDROID_NDK_HOST:-linux-x86_64}"

bash "$CI_PROJECT_DIR/scripts/build-android-sdl3.sh" "$ABI" \
    --deps-root "$DEPS_ROOT" \
    --ndk "$ANDROID_NDK" \
    --qt-android "$QT_ANDROID" \
    --java-home "$JAVA_HOME" \
    --android-home "$ANDROID_HOME"

[ -d "$DEPS_ROOT/ffmpeg/$ABI/lib/pkgconfig" ] || { echo "FFmpeg pkgconfig missing after build" >&2; exit 1; }
[ -d "$DEPS_ROOT/sdl3/$ABI/lib/cmake" ] || { echo "SDL3 cmake dir missing after build" >&2; exit 1; }

export PKG_CONFIG_PATH="$DEPS_ROOT/ffmpeg/$ABI/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$DEPS_ROOT/ffmpeg/$ABI/lib/pkgconfig"
export CMAKE_PREFIX_PATH="$DEPS_ROOT/ffmpeg/$ABI:$DEPS_ROOT/sdl3/$ABI:$QT_ANDROID${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

python3 -u ci-utilities/run-ci-build.py \
    --project "$CI_PROJECT_NAME" \
    --branch "$CI_COMMIT_REF_NAME" \
    --platform Android/Qt6/Shared \
    --extra-cmake-args=-DBUILD_WITH_QT6=ON \
    --extra-cmake-args=-DBUILD_EXAMPLES=OFF \
    --extra-cmake-args=-DANDROID_ABI="$ABI" \
    --extra-cmake-args=-DANDROID_PLATFORM=android-28
