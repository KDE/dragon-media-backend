#!/bin/bash
# SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
# SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

set -e

CONFIG="${1:?Usage: start-pipewire.sh <config-file>}"

if [ -z "$XDG_RUNTIME_DIR" ]; then
    echo "XDG_RUNTIME_DIR is not set cannot start PipeWire"
    exit 1
fi

mkdir -p "$XDG_RUNTIME_DIR"

killall pipewire wireplumber 2>/dev/null || true
rm -f "$XDG_RUNTIME_DIR/pipewire-0" "$XDG_RUNTIME_DIR/pipewire-0.lock"
rm -rf "$XDG_RUNTIME_DIR/pulse"

pipewire -c "$CONFIG" > /tmp/dragon-pipewire.log 2>&1 &
PW_PID=$!

for i in $(seq 1 100); do
    if [ -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
        break
    fi
    if ! kill -0 "$PW_PID" 2>/dev/null; then
        echo "PipeWire exited unexpectedly during startup"
        cat /tmp/dragon-pipewire.log 2>/dev/null
        exit 1
    fi
    sleep 0.1
done

if [ ! -S "$XDG_RUNTIME_DIR/pipewire-0" ]; then
    echo "PipeWire native socket did not appear within 10 seconds"
    cat /tmp/dragon-pipewire.log 2>/dev/null
    kill "$PW_PID" 2>/dev/null || true
    exit 1
fi

if ! command -v wireplumber >/dev/null 2>&1; then
    echo "WirePlumber not installed; skipping session manager (static config objects only)"
    echo "PipeWire started without WirePlumber (PID $PW_PID)"
    exit 0
fi

wireplumber > /tmp/dragon-wireplumber.log 2>&1 &
WP_PID=$!

for i in $(seq 1 50); do
    if ! kill -0 "$WP_PID" 2>/dev/null; then
        echo "WirePlumber exited unexpectedly during startup"
        cat /tmp/dragon-wireplumber.log 2>/dev/null
        exit 1
    fi
    if pw-cli list-objects Client 2>/dev/null | grep -q "WirePlumber"; then
        break
    fi
    sleep 0.1
done

for i in $(seq 1 100); do
    if [ -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
        break
    fi
    if ! kill -0 "$PW_PID" 2>/dev/null; then
        echo "PipeWire exited while waiting for pulse socket"
        exit 1
    fi
    sleep 0.1
done

if [ ! -S "$XDG_RUNTIME_DIR/pulse/native" ]; then
    echo "PipeWire PulseAudio socket did not appear within 10 seconds"
    cat /tmp/dragon-pipewire.log 2>/dev/null
    kill "$WP_PID" "$PW_PID" 2>/dev/null || true
    exit 1
fi

# just waiting for the socket to exist isn't enough; WirePlumber still
# needs to register the null audio sink node so playback streams can be
# created. Poll until the auto_null sink appears.
for i in $(seq 1 100); do
    pw-cli list-objects Node 2>/dev/null | grep -q "node.name.*auto_null" && break
    if ! kill -0 "$PW_PID" 2>/dev/null; then
        echo "PipeWire exited while waiting for null audio sink"
        cat /tmp/dragon-pipewire.log 2>/dev/null
        exit 1
    fi
    sleep 0.1
done

if ! pw-cli list-objects Node 2>/dev/null | grep -q "node.name.*auto_null"; then
    echo "PipeWire null audio sink did not appear within 10 seconds"
    cat /tmp/dragon-pipewire.log 2>/dev/null
    cat /tmp/dragon-wireplumber.log 2>/dev/null
    kill "$WP_PID" "$PW_PID" 2>/dev/null || true
    exit 1
fi

echo "PipeWire + WirePlumber started (PIDs $PW_PID, $WP_PID)"
exit 0
