#!/bin/bash

# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
#
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

# Patch QCoro to fix ECM compatibility - comment out ecm_generate_pri_file block entirely

QCORO_DIR="$1"
CMAKE_FILE="${QCORO_DIR}/cmake/AddQCoroLibrary.cmake"

if [ -f "$CMAKE_FILE" ]; then
    # Create a backup and comment out the entire ecm_generate_pri_file block
    cp "$CMAKE_FILE" "${CMAKE_FILE}.bak"
    
    # Use awk to comment out from ecm_generate_pri_file to its closing parenthesis
    awk '
    /ecm_generate_pri_file/ {
        print "# Disabled for ECM compatibility: " $0
        getline
        while ($0 !~ /\)$/) {
            print "# Disabled: " $0
            getline
        }
        print "# Disabled: " $0
        next
    }
    { print }
    ' "$CMAKE_FILE" > "${CMAKE_FILE}.tmp" && mv "${CMAKE_FILE}.tmp" "$CMAKE_FILE"
fi
