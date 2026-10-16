#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
#
# CI artifact build for Android (android_qt611 job).
#
# FFmpeg, SDL3 and kissfft are cross-compiled by KDE Craft from the
# upstream craft-blueprints-kde and installed into the image's
# Qt-for-Android SDK prefix, which run-ci-build.py adds to
# CMAKE_PREFIX_PATH and PKG_CONFIG_PATH automatically. Qt, KIO and QCoro
# come from the CI dependency system (.kde-ci.yml) as before.

set -euo pipefail

ABI=arm64-v8a
QT_ANDROID=/home/user/android-arm64-clang

: "${ANDROID_NDK:?the CI image must export ANDROID_NDK}"
: "${ANDROID_HOME:?the CI image must export ANDROID_HOME}"
: "${JAVA_HOME:?the CI image must export JAVA_HOME}"
: "${CI_PROJECT_DIR:?only for CI use}"

git config --global --add safe.directory "$CI_PROJECT_DIR" 2>/dev/null || true

bash "$CI_PROJECT_DIR/ci/craft-android-deps.sh" \
    --target android-arm64-clang \
    --root "$QT_ANDROID" \
    libs/ffmpeg libs/libsdl3 libs/kissfft

python3 -u ci-utilities/run-ci-build.py \
    --project "$CI_PROJECT_NAME" \
    --branch "$CI_COMMIT_REF_NAME" \
    --platform Android/Qt6/Shared \
    --extra-cmake-args=-DBUILD_WITH_QT6=ON \
    --extra-cmake-args=-DBUILD_EXAMPLES=OFF \
    --extra-cmake-args=-DANDROID_ABI="$ABI" \
    --extra-cmake-args=-DANDROID_PLATFORM=android-28
