#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# Usage:
#   build-sdl3-qtactivity-jar.sh --sdl-source DIR --qt-android DIR --output FILE.jar
#       [--java-home DIR] [--android-home DIR]
#

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SDL_SOURCE=""
QT_ANDROID=""
OUTPUT=""
JAVA_HOME_ARG=""
ANDROID_HOME_ARG=""

usage() {
    sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        --sdl-source)    SDL_SOURCE=${2:?--sdl-source needs a value}; shift ;;
        --qt-android)    QT_ANDROID=${2:?--qt-android needs a value}; shift ;;
        --output)        OUTPUT=${2:?--output needs a value}; shift ;;
        --java-home)     JAVA_HOME_ARG=${2:?--java-home needs a value}; shift ;;
        --android-home)  ANDROID_HOME_ARG=${2:?--android-home needs a value}; shift ;;
        -h|--help)       usage ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
    shift
done

[ -n "$SDL_SOURCE" ] || { echo "usage: build-sdl3-qtactivity-jar.sh --sdl-source DIR … — see --help" >&2; exit 1; }
[ -n "$QT_ANDROID" ] || { echo "--qt-android is required (the Qt-for-Android craft prefix)" >&2; exit 1; }
[ -n "$OUTPUT" ] || { echo "--output is required (the .jar file to produce)" >&2; exit 1; }

SDL_JAVA_ORG="$SDL_SOURCE/android-project/app/src/main/java/org"
[ -d "$SDL_JAVA_ORG" ] || { echo "not an SDL source tree: $SDL_SOURCE (no android-project java sources)" >&2; exit 1; }

PATCH="$REPO_ROOT/scripts/sdl3-qtactivity-qt-hybrid.patch"
[ -f "$PATCH" ] || { echo "patch not found: $PATCH" >&2; exit 1; }

JAVA_HOME="${JAVA_HOME_ARG:-${JAVA_HOME:?jar build needs a JDK: pass --java-home or export JAVA_HOME}}"
ANDROID_HOME="${ANDROID_HOME_ARG:-${ANDROID_HOME:?jar build needs the SDK: pass --android-home or export ANDROID_HOME}}"
JAVAC="$JAVA_HOME/bin/javac"
JAR_TOOL="$JAVA_HOME/bin/jar"

ANDROID_JAR="$(find "$ANDROID_HOME/platforms" -maxdepth 2 -name android.jar | sort -V | tail -1)"
[ -n "$ANDROID_JAR" ] || { echo "no android.jar under $ANDROID_HOME/platforms" >&2; exit 1; }

QT_ANDROID_JAR="$QT_ANDROID/jar/Qt6Android.jar"
QT_BINDINGS_SRC="$QT_ANDROID/src/android/java/src"
[ -f "$QT_ANDROID_JAR" ] || { echo "missing $QT_ANDROID_JAR" >&2; exit 1; }
[ -d "$QT_BINDINGS_SRC" ] || { echo "missing $QT_BINDINGS_SRC" >&2; exit 1; }

if [ -f "$OUTPUT" ]; then
    echo "sdl3 qtactivity jar already exists at $OUTPUT — skipping jar build"
    exit 0
fi

STAGE="$(mktemp -d -t sdl3-qtactivity-XXXXXX)"
CLASSES="$(mktemp -d -t sdl3-qtactivity-classes-XXXXXX)"
trap 'rm -rf "$STAGE" "$CLASSES"' EXIT

cp -r "$SDL_JAVA_ORG" "$STAGE/"
patch -d "$STAGE" -p1 --no-backup-if-mismatch < "$PATCH" || {
    echo ""
    echo "sdl3-qtactivity-qt-hybrid.patch does not apply to $SDL_SOURCE." >&2
    echo "Rebase the patch (see its header comment) and re-run." >&2
    exit 1
}

"$JAVAC" --release 17 -encoding UTF-8 \
    -classpath "$ANDROID_JAR:$QT_ANDROID_JAR" \
    -sourcepath "$QT_BINDINGS_SRC" \
    -implicit:none \
    -d "$CLASSES" \
    "$STAGE"/org/libsdl/app/*.java
mkdir -p "$(dirname "$OUTPUT")"
"$JAR_TOOL" --create --file "$OUTPUT" -C "$CLASSES" .

echo "## sdl3 qtactivity jar installed to $OUTPUT"
