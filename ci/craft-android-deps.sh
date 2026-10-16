#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

set -euo pipefail

TARGET=""
ROOT=""
DEPS_ONLY=0
PACKAGES=()

usage() {
    sed -n '/^# Usage:/,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while [ $# -gt 0 ]; do
    case $1 in
        --target)     TARGET=${2:?--target needs a value}; shift ;;
        --root)       ROOT=${2:?--root needs a value}; shift ;;
        --deps-only)  DEPS_ONLY=1 ;;
        -h|--help)    usage ;;
        *)            PACKAGES+=("$1") ;;
    esac
    shift
done

[ -n "$TARGET" ] || { echo "usage: craft-android-deps.sh --target <craft-target> --root <craft-root> … — see --help" >&2; exit 1; }
[ -n "$ROOT" ] || { echo "--root is required (the craft root to install into)" >&2; exit 1; }
[ "${#PACKAGES[@]}" -gt 0 ] || { echo "at least one package is required, e.g. kde/libs/dragon-media-backend" >&2; exit 1; }
: "${ANDROID_NDK:?craft needs ANDROID_NDK exported}"
mkdir -p "$ROOT"

CRAFT_CWD="$(dirname "$ROOT")"
case $TARGET in
  android-arm64-clang)  ABI=arm64 ;;
  android-x86_64-clang) ABI=x86_64 ;;
  *) echo "unknown craft target: $TARGET (expected android-<abi>-clang)" >&2; exit 1 ;;
esac
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

[$TARGET]
General/ABI = android-clang-$ABI
General/AndroidAPI = 28
Compile/BuildType = MinSizeRel
EOF

ACTION=()
if [ "$DEPS_ONLY" = 1 ]; then
    # install only the dependencies of the named packages, not the packages
    # themselves (used with kde/libs/dragon-media-backend to get its full
    # dependency graph without building dragon itself)
    ACTION=(--install-deps)
fi

python3 craftmaster/CraftMaster.py --config CraftConfig.ini --target "$TARGET" -c -i craft
python3 craftmaster/CraftMaster.py --config CraftConfig.ini --target "$TARGET" -c "${ACTION[@]}" "${PACKAGES[@]}"
