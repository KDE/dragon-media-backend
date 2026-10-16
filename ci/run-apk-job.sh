#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# Usage:
#   run-apk-job.sh <arm64-v8a|x86_64> [--project-dir DIR] [--deps-root DIR] [--qt-android DIR]
#
# Builds the dragonqml example APK for one Android ABI. Every native
# dependency (Qt, KF6/KIO, QCoro, FFmpeg, SDL3, kissfft) is installed by
# KDE Craft from the upstream craft-blueprints-kde, via the dependency
# graph of kde/libs/dragon-media-backend. The SDL3 QtActivity java glue
# jar is built from the SDL3 sources Craft unpacked
# (see scripts/build-sdl3-qtactivity-jar.sh).

set -euo pipefail

ABI=""
PROJECT_DIR=${CI_PROJECT_DIR:-}
DEPS_ROOT=${DRAGON_ANDROID_DEPS_ROOT:-}
QT_ANDROID_ARG=""
IMAGE_ARM64_PREFIX=/home/user/android-arm64-clang

usage() {
    sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        arm64-v8a|x86_64) ABI=$1 ;;
        --project-dir) PROJECT_DIR=${2:?--project-dir needs a value}; shift ;;
        --deps-root)    DEPS_ROOT=${2:?--deps-root needs a value}; shift ;;
        --qt-android)   QT_ANDROID_ARG=${2:?--qt-android needs a value}; shift ;;
        -h|--help)      usage ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

[ -n "$ABI" ] || { echo "usage: ci/run-apk-job.sh <abi> [options] — see --help" >&2; exit 1; }
if [ -z "$PROJECT_DIR" ]; then
    PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
fi
if [ -z "$DEPS_ROOT" ]; then
    DEPS_ROOT="$PROJECT_DIR/.ci-android-deps"
fi

case $ABI in
  arm64-v8a) CRAFT_TARGET=android-arm64-clang; PRESET=android-arm64-example; BUILD_DIR=build-android-arm64 ;;
  x86_64)    CRAFT_TARGET=android-x86_64-clang; PRESET=android-x86_64-example; BUILD_DIR=build-android-x86_64 ;;
esac

: "${ANDROID_NDK:?the CI image must export ANDROID_NDK}"
: "${ANDROID_HOME:?the CI image must export ANDROID_HOME}"
: "${JAVA_HOME:?the CI image must export JAVA_HOME}"
export QT_HOST_PATH=${QT_HOST_PATH:-/opt/nativetooling}
export QT_HOST_PATH_CMAKE_DIR=${QT_HOST_PATH_CMAKE_DIR:-$QT_HOST_PATH/lib/cmake}
export ECM_DIR=${ECM_DIR:-$QT_HOST_PATH/share/ECM/cmake}
[ -d "$QT_HOST_PATH" ] || { echo "QT_HOST_PATH does not exist: $QT_HOST_PATH" >&2; exit 1; }
[ -d "$ECM_DIR" ] || { echo "ECM_DIR does not exist: $ECM_DIR" >&2; exit 1; }

if [ -n "$QT_ANDROID_ARG" ]; then
    QT_ANDROID="$QT_ANDROID_ARG"
elif [ "$ABI" = arm64-v8a ] && [ -d "$IMAGE_ARM64_PREFIX/lib/cmake/Qt6" ]; then
    # the CI image ships a ready-made Qt-for-Android craft root
    QT_ANDROID="$IMAGE_ARM64_PREFIX"
else
    QT_ANDROID="$DEPS_ROOT/craft/$CRAFT_TARGET"
fi

if [ "$ABI" = arm64-v8a ]; then
    export DRAGON_QT_ANDROID_ARM64="$QT_ANDROID"
else
    export DRAGON_QT_ANDROID_X86_64="$QT_ANDROID"
fi

git config --global --add safe.directory "$PROJECT_DIR" 2>/dev/null || true

bash "$PROJECT_DIR/ci/craft-android-deps.sh" \
    --target "$CRAFT_TARGET" \
    --root "$QT_ANDROID" \
    --deps-only kde/libs/dragon-media-backend

# Qt modules the QML example needs beyond dragon's own requirements
bash "$PROJECT_DIR/ci/craft-android-deps.sh" \
    --target "$CRAFT_TARGET" \
    --root "$QT_ANDROID" \
    libs/qt6/qtsvg libs/qt6/qttools libs/qt6/qtshadertools

# SDL3 java glue: built from the same SDL3 sources Craft unpacked, so the
# jar matches the native libSDL3.so in the craft root
SDL_ANDROID_PROJECT="$(find "$QT_ANDROID/build/libs/libsdl3" -type d -name android-project | head -1)"
if [ -z "$SDL_ANDROID_PROJECT" ]; then
    echo "SDL3 sources not found under $QT_ANDROID/build/libs/libsdl3 — expected libs/libsdl3 to have been built from source" >&2
    exit 1
fi
bash "$PROJECT_DIR/scripts/build-sdl3-qtactivity-jar.sh" \
    --sdl-source "$(dirname "$SDL_ANDROID_PROJECT")" \
    --qt-android "$QT_ANDROID" \
    --output "$QT_ANDROID/jar/SDL3-qtactivity-qt.jar" \
    --java-home "$JAVA_HOME" \
    --android-home "$ANDROID_HOME"

cd "$PROJECT_DIR"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"

APK_DIR="$PROJECT_DIR/$BUILD_DIR/examples/qml/android-build"
APK="$(find "$APK_DIR" -maxdepth 1 -name '*.apk' | sort | tail -1)"
[ -n "$APK" ] || { echo "no APK produced in $APK_DIR" >&2; exit 1; }

KEYSTORE="$HOME/.android/dragon-debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    "$JAVA_HOME/bin/keytool" -genkeypair \
        -keystore "$KEYSTORE" -storepass dragon -keypass dragon \
        -alias dragondebug -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
APKSIGNER="$(find "$ANDROID_HOME/build-tools" -maxdepth 2 -name apksigner | sort -V | tail -1)"
[ -n "$APKSIGNER" ] || { echo "no apksigner under $ANDROID_HOME/build-tools" >&2; exit 1; }
"$APKSIGNER" sign --ks "$KEYSTORE" --ks-pass pass:dragon --key-pass pass:dragon "$APK"
"$APKSIGNER" verify --print-certs "$APK" | head -3

echo "## APK ready: $APK"
ls -la "$APK_DIR"
