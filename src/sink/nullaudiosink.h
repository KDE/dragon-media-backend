/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "audiosink.h"
#include "dragonmediabackend_export.h"

#include <expected>
#include <memory>

class DRAGONMEDIABACKEND_EXPORT NullAudioSink : public AudioSink
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(NullAudioSink)

public:
    explicit NullAudioSink(QObject *parent = nullptr);
    ~NullAudioSink() override = default;

    [[nodiscard]] bool probe() override;
    [[nodiscard]] std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit = {}) override;

    // The null sink never has a device: stream creation always fails by
    // contract (with the existing missing-plugin error) and there is no
    // timer-driven playback. The player sees isDeviceOpen() == false and
    // refuses playback-state transitions.
};
