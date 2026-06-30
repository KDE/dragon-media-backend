/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <DragonMultimedia/dragonfftframe.h>
#include <DragonMultimedia/dragonplayer.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

template<typename T>
class DragonPipe;
struct DragonFftBlock;
class DragonFftProcessor;

class DragonFftPipeline
{
public:
    explicit DragonFftPipeline(DragonPipe<DragonFftBlock> *pipe);
    ~DragonFftPipeline();

    DragonFftPipeline(const DragonFftPipeline &) = delete;
    DragonFftPipeline &operator=(const DragonFftPipeline &) = delete;
    DragonFftPipeline(DragonFftPipeline &&) = delete;
    DragonFftPipeline &operator=(DragonFftPipeline &&) = delete;

    void setSampleRate(int sampleRate);
    void setChannelCount(int channels);
    void setMode(DragonPlayer::FftMode mode);
    void setFftRate(int rate);
    [[nodiscard]] DragonPlayer::FftMode mode() const;

    void stop();

    void restart();

    using FrameCallback = std::function<void(DragonFftFrame)>;
    void setFrameCallback(FrameCallback cb);

private:
    static constexpr size_t kBufferCapacity = 65536;

    void ensureInfrastructure();
    void teardown();
    void startThread();
    void stopThread();
    void restartThread();

    std::unique_ptr<DragonFftProcessor> m_fftProcessor;

    std::jthread m_fftThread;

    DragonPipe<DragonFftBlock> *const m_fftPipe;

    DragonPlayer::FftMode m_currentMode = DragonPlayer::FftMode::Off;
    int m_fftRate = 60;
    bool m_infrastructureCreated = false;

    FrameCallback m_frameCallback;
};
