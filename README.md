<!--
SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>

SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
-->

Dragon Media Backend
------------------
![A mythical beast named Qilin or Kirin](logo.svg)

A Qt C++ library intended for desktop music players. [API documentation](https://eean.dev/dragon-media-backend/api/) is
available.

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
Dragon Media Backend implements a rather simple pipeline of input -> decoding -> sink (+FFT). The sink drives the
pipeline so we don't have to.

The plan is to keep simple; more complicated DSP features will be implemented via PipeWire and perhaps exclusive to it.

This library is use-case oriented, currently that means just music players. It differs from Qt Multimedia and Phonon in
that it will never provide raw PCM output/input, direct graph manipulation etc. 

More specific plans available in the [todo file](todo.md).