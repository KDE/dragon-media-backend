#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
SDL_PIN=c8d08ea2dfd5eb854407124e7fe420d2fe98cfbe

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS_ROOT=${DRAGON_ANDROID_DEPS_ROOT:-$HOME/Android/deps}
NDK=${ANDROID_NDK:-}
SDL_PROJECT=""
SDL_REVISION=$SDL_PIN
QT_ANDROID_ARG=""
JAVA_HOME_ARG=""
ANDROID_HOME_ARG=""
ABI=""

usage() {
    sed -n '/^# Usage:/,/^# SDL source pin/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        arm64-v8a|x86_64) ABI=$1 ;;
        --deps-root)     DEPS_ROOT=${2:?--deps-root needs a value}; shift ;;
        --ndk)           NDK=${2:?--ndk needs a value}; shift ;;
        --sdl-source)    SDL_PROJECT=${2:?--sdl-source needs a value}; shift ;;
        --sdl-revision)  SDL_REVISION=${2:?--sdl-revision needs a value}; shift ;;
        --qt-android)    QT_ANDROID_ARG=${2:?--qt-android needs a value}; shift ;;
        --java-home)     JAVA_HOME_ARG=${2:?--java-home needs a value}; shift ;;
        --android-home)  ANDROID_HOME_ARG=${2:?--android-home needs a value}; shift ;;
        -h|--help)       usage ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

[ -n "$ABI" ] || { echo "usage: build-android-sdl3.sh <abi> [options] — see --help" >&2; exit 1; }
[ -n "$NDK" ] || { echo "no NDK: pass --ndk or export ANDROID_NDK" >&2; exit 1; }

PREFIX="$DEPS_ROOT/sdl3/$ABI"
TMP="$DEPS_ROOT/tmp/sdl3-$ABI"
JAR_DIR="$DEPS_ROOT/sdl3"
API=28

if [ -z "$SDL_PROJECT" ]; then
    DEFAULT_SRC="$REPO_ROOT/../research/SDL"
    if [ -d "$DEFAULT_SRC" ]; then
        SDL_PROJECT="$DEFAULT_SRC"
    else
        SDL_PROJECT="$DEPS_ROOT/src/SDL"
        PIN_MARKER="$SDL_PROJECT/.dragon-pin"
        if [ ! -d "$SDL_PROJECT" ]; then
            echo "## fetching SDL @ $SDL_REVISION"
            mkdir -p "$(dirname "$SDL_PROJECT")"
            git clone https://github.com/libsdl-org/SDL "$SDL_PROJECT"
            git -C "$SDL_PROJECT" checkout "$SDL_REVISION"
            printf '%s\n' "$SDL_REVISION" > "$PIN_MARKER"
        elif [ -f "$PIN_MARKER" ] && [ "$(cat "$PIN_MARKER")" != "$SDL_REVISION" ]; then
            echo "## fetched clone is stale — checking out $SDL_REVISION"
            git -C "$SDL_PROJECT" fetch origin
            git -C "$SDL_PROJECT" checkout "$SDL_REVISION"
            printf '%s\n' "$SDL_REVISION" > "$PIN_MARKER"
        fi
    fi
fi

if [ ! -d "$SDL_PROJECT/.git" ]; then
    echo "SDL source at $SDL_PROJECT is not a git checkout" >&2
    exit 1
fi

PATCH="$REPO_ROOT/scripts/sdl3-qtactivity-qt-hybrid.patch"
[ -f "$PATCH" ] || { echo "patch not found: $PATCH" >&2; exit 1; }

SDL_REV="$(git -C "$SDL_PROJECT" rev-parse --short HEAD)"
REV_FILE="$PREFIX/.sdl-revision"

case $ABI in
  arm64-v8a) ANDROID_ABI=arm64-v8a; QT_ANDROID_ENV=DRAGON_QT_ANDROID_ARM64 ;;
  x86_64)    ANDROID_ABI=x86_64;    QT_ANDROID_ENV=DRAGON_QT_ANDROID_X86_64 ;;
  *) echo "unknown ABI: $ABI" >&2; exit 1 ;;
esac

NATIVE_OK=0
if [ -f "$PREFIX/lib/cmake/SDL3/SDL3Config.cmake" ] || \
   [ -f "$PREFIX/lib64/cmake/SDL3/SDL3Config.cmake" ]; then
  if [ -f "$REV_FILE" ] && [ "$(cat "$REV_FILE")" = "$SDL_REV" ]; then
    NATIVE_OK=1
  fi
fi

if [ "$NATIVE_OK" = 0 ]; then
  mkdir -p "$TMP"
  BUILD="$TMP/build"

  cmake -S "$SDL_PROJECT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ANDROID_ABI \
    -DANDROID_PLATFORM=android-$API \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_DISABLE_INSTALL_DOCS=ON

  cmake --build "$BUILD" -j"$(nproc)"
  cmake --install "$BUILD"
  printf '%s\n' "$SDL_REV" > "$REV_FILE"
  echo "## sdl3 $ABI ($SDL_REV) installed to $PREFIX"
else
  echo "sdl3 $ABI ($SDL_REV) already built at $PREFIX — skipping native build"
fi

JAR="$JAR_DIR/SDL3-$SDL_REV.jar"
mkdir -p "$JAR_DIR"
if [ -f "$JAR" ]; then
  echo "sdl3 java jar $JAR already exists — skipping jar build"
else
  JAVA_HOME="${JAVA_HOME_ARG:-${JAVA_HOME:?jar build needs a JDK: pass --java-home or export JAVA_HOME}}"
  ANDROID_HOME="${ANDROID_HOME_ARG:-${ANDROID_HOME:?jar build needs the SDK: pass --android-home or export ANDROID_HOME}}"
  JAVAC="$JAVA_HOME/bin/javac"
  JAR_TOOL="$JAVA_HOME/bin/jar"

  ANDROID_JAR="$(find "$ANDROID_HOME/platforms" -maxdepth 2 -name android.jar \
    | sort -V | tail -1)"
  [ -n "$ANDROID_JAR" ] || { echo "no android.jar under $ANDROID_HOME/platforms" >&2; exit 1; }

  QT_ANDROID="${QT_ANDROID_ARG:-${!QT_ANDROID_ENV:?}}"
  QT_ANDROID_JAR="$QT_ANDROID/jar/Qt6Android.jar"
  QT_BINDINGS_SRC="$QT_ANDROID/src/android/java/src"
  [ -f "$QT_ANDROID_JAR" ] || { echo "missing $QT_ANDROID_JAR" >&2; exit 1; }
  [ -d "$QT_BINDINGS_SRC" ] || { echo "missing $QT_BINDINGS_SRC" >&2; exit 1; }

  STAGE="$TMP/sdl-java"
  CLASSES="$TMP/jar-classes"
  rm -rf "$STAGE" "$CLASSES"
  mkdir -p "$STAGE" "$CLASSES"
  cp -r "$SDL_PROJECT/android-project/app/src/main/java/org" "$STAGE/"
  patch -d "$STAGE" -p1 --no-backup-if-mismatch < "$PATCH" || {
    echo ""
    echo "sdl3-qtactivity-qt-hybrid.patch does not apply to $SDL_PROJECT @ $SDL_REV." >&2
    echo "Rebase the patch (see its header comment) and re-run." >&2
    exit 1
  }

  "$JAVAC" --release 17 -encoding UTF-8 \
    -classpath "$ANDROID_JAR:$QT_ANDROID_JAR" \
    -sourcepath "$QT_BINDINGS_SRC" \
    -implicit:none \
    -d "$CLASSES" \
    "$STAGE"/org/libsdl/app/*.java
  "$JAR_TOOL" --create --file "$JAR" -C "$CLASSES" .

  find "$JAR_DIR" -maxdepth 1 -name 'SDL3-*.jar' \
    ! -name "SDL3-$SDL_REV.jar" ! -name 'SDL3-current.jar' -delete
  echo "## sdl3 java jar (patched SDLActivity) installed to $JAR"
fi

ln -sfn "$(basename "$JAR")" "$JAR_DIR/SDL3-current.jar"

find "$PREFIX" -name "libSDL3.so" -o -name "SDL3Config.cmake" | head -5
