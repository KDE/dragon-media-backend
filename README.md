<!--
SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>

SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
-->

Dragon Media Backend
------------------
![A mythical beast named Qilin or Kirin](logo.svg)

A Qt C++ library intended for desktop music players.

## Features
 - Can play any audio ffmpeg decodes (~everything)
 - Plugin system for audio output, with support for:
    - PipeWire
    - PulseAudio
    - SDL 
 - Calculates FFTs for audio visualizations
 - Can play music over KDE's KIO system
 - Supports ICY metadata for radio streams
 - Volume API keeps in sync with Linux desktop application volume
 - A QWidget-based desktop example app and a QML-based Android example app

## Prerequisites

Building the library requires the following:

 - A C++23-capable compiler
 - CMake >= 3.25
 - pkg-config
 - Qt >= 6.11
 - QCoro 6*
 - FFmpeg
 - kissfft*
 - SDL3*
 - KDE Framework libraries:
    - Extra CMake Modules
    - KIO
    - KCoreAddons
    - KI18n

And the optional but recommended audio sinks:
 - PipeWire
 - PulseAudio

[*] Fetched automatically by CMake if not found on the system.

## Tech Stack
Its tech stack relies on Qt, modern C++ jthreads and ffmpeg for decoding audio files. It primarily targets the Linux
desktop with PulseAudio and PipeWire audio sinks, but it also has SDL support. Since SDL has audio sinks on ~everything,
and the rest of the tech stack is cross-platform, probably it can run on anything.

Decoding and FFT calculations are done on their own threads; audio data transfers are via lockless queue pipes. 

## Plans
Dragon Media Backend implements a rather simple pipeline of input -> decoding -> sink (+FFT). The plan is to keep it
that way; more complicated DSP features will be implemented via PipeWire and exclusive to it.

Qt-based video playback functionality is a logical next step, but audio remains the focus for now.
