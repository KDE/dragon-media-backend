/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonaudiosink.h"

#include <QVariant>

#include <atomic>

struct pw_thread_loop;
struct pw_stream;
struct spa_hook;

class DragonPipeWireAudioSink : public DragonAudioSink
{
    Q_OBJECT

public:
    explicit DragonPipeWireAudioSink(QObject *parent, const QVariantList &args);
    ~DragonPipeWireAudioSink() override;

    void open(int sampleRate, int channels) override;
    void close() override;
    void pause() override;
    void resume() override;
    void setGain(float linearGain) override;
    void clearStream() override;
    [[nodiscard]] int64_t deviceQueuedSamples() const override;

    [[nodiscard]] int audioBufferFrames() const override;
    [[nodiscard]] int audioBufferUs() const override;

    [[nodiscard]] bool isDeviceOpen() const override;
    [[nodiscard]] bool isPaused() const override;

    // Called from the pw_thread_loop callback thread public for the static events struct
    static void onProcess(void *userdata);

private:
    struct PwState;
    std::unique_ptr<PwState> m_pw;

    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_open{false};

    float m_cachedGain = 1.0f;
};
