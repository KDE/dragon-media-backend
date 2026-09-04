#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
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
[ -d "$PROJECT_DIR/scripts" ] || { echo "not a dragon-media-backend checkout: $PROJECT_DIR" >&2; exit 1; }
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

export DRAGON_ANDROID_DEPS_ROOT="$DEPS_ROOT"
mkdir -p "$DEPS_ROOT"

git config --global --add safe.directory "$PROJECT_DIR" 2>/dev/null || true

if [ -n "$QT_ANDROID_ARG" ]; then
    QT_ANDROID="$QT_ANDROID_ARG"
elif [ "$ABI" = arm64-v8a ] && [ -d "$IMAGE_ARM64_PREFIX/lib/cmake/Qt6" ]; then
    QT_ANDROID="$IMAGE_ARM64_PREFIX"
else
    QT_ANDROID="$DEPS_ROOT/craft/$CRAFT_TARGET"
fi

QT_CMAKE_DIR="$QT_ANDROID/lib/cmake/Qt6"
if [ ! -d "$QT_CMAKE_DIR" ]; then
    CRAFT_PACKAGES=(libs/qt6/qtbase libs/qt6/qtdeclarative libs/qt6/qtsvg \
        libs/qt6/qttools libs/qt6/qtshadertools qt-libs/qcoro \
        kde/frameworks/tier3/kio)
elif [ ! -d "$QT_ANDROID/lib/cmake/KF6KIO" ]; then
    CRAFT_PACKAGES=(kde/frameworks/tier3/kio)
else
    CRAFT_PACKAGES=()
fi

if [ "${#CRAFT_PACKAGES[@]}" -gt 0 ]; then
    if [ "$QT_ANDROID" = "$IMAGE_ARM64_PREFIX" ]; then
        CRAFT_CWD="$(dirname "$IMAGE_ARM64_PREFIX")"
    else
        CRAFT_CWD="$DEPS_ROOT/craft"
    fi
    mkdir -p "$CRAFT_CWD"
    cd "$CRAFT_CWD"
    [ -d craftmaster ] || git clone --depth=1 https://invent.kde.org/packaging/craftmaster.git
    [ -f CraftConfig.ini ] || cat > CraftConfig.ini <<EOF
[General]
Branch = master
ShallowClone = True

[GeneralSettings]
Packager/RepositoryUrl = https://files.kde.org/craft/Qt6/
Packager/CacheVersion = 26.05
Packager/UseCache = True
Packager/CreateCache = False
ContinuousIntegration/Enabled = False
General/KFHostToolingVersion = 6
Paths/DownloadDir = $HOME/.cache/dragon-craft-downloads
Paths/KDEGitDir = $HOME/.cache/dragon-craft-downloads/git

[$CRAFT_TARGET]
General/ABI = android-clang-${ABI/arm64-v8a/arm64}
General/AndroidAPI = 28
Compile/BuildType = MinSizeRel
EOF
    python3 craftmaster/CraftMaster.py --config CraftConfig.ini --target "$CRAFT_TARGET" -c -i craft
    python3 craftmaster/CraftMaster.py --config CraftConfig.ini --target "$CRAFT_TARGET" -c "${CRAFT_PACKAGES[@]}"
fi

[ -d "$QT_CMAKE_DIR" ] || { echo "Qt-for-Android cmake dir missing after craft: $QT_CMAKE_DIR" >&2; exit 1; }
[ -d "$QT_ANDROID/lib/cmake/KF6KIO" ] || { echo "KF6KIO missing after craft in $QT_ANDROID" >&2; exit 1; }

if [ "$ABI" = arm64-v8a ]; then
    export DRAGON_QT_ANDROID_ARM64="$QT_ANDROID"
else
    export DRAGON_QT_ANDROID_X86_64="$QT_ANDROID"
fi

"$PROJECT_DIR/scripts/build-android-ffmpeg.sh" "$ABI" \
    --deps-root "$DEPS_ROOT" \
    --ndk "$ANDROID_NDK" \
    --ndk-host "${ANDROID_NDK_HOST:-linux-x86_64}"

"$PROJECT_DIR/scripts/build-android-sdl3.sh" "$ABI" \
    --deps-root "$DEPS_ROOT" \
    --ndk "$ANDROID_NDK" \
    --qt-android "$QT_ANDROID" \
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
