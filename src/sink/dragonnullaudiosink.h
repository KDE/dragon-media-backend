/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonaudiosink.h"

class DragonNullAudioSink : public DragonAudioSink
{
    Q_OBJECT

public:
    explicit DragonNullAudioSink(QObject *parent = nullptr);

    [[nodiscard]] bool probe() override;
    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    [[nodiscard]] int64_t deviceQueuedSamples() const override;
    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;
    void clearStream() override;
};
