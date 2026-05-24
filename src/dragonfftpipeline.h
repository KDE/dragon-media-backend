/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonplayer.h>

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
    explicit DragonFftPipeline();
    ~DragonFftPipeline();

    DragonFftPipeline(const DragonFftPipeline &) = delete;
    DragonFftPipeline &operator=(const DragonFftPipeline &) = delete;
    DragonFftPipeline(DragonFftPipeline &&) = delete;
    DragonFftPipeline &operator=(DragonFftPipeline &&) = delete;

    void ensureInfrastructure(DragonPipe<DragonFftBlock> *pipe, DragonPlayer::FftMode mode);

    void teardown();

    void setSampleRate(int sampleRate);
    void setChannelCount(int channels);
    void setMode(DragonPlayer::FftMode mode);
    void setFftRate(int rate);
    [[nodiscard]] DragonPlayer::FftMode mode() const;

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    void restartThread();

    using FrameCallback = std::function<void(DragonFftFrame)>;
    void setFrameCallback(FrameCallback cb);
    [[nodiscard]] bool hasInfrastructure() const;

private:
    static constexpr size_t kBufferCapacity = 65536;

    void ensureInfrastructure();
    void startThread();
    void stopThread();

    std::unique_ptr<DragonFftProcessor> m_fftProcessor;

    std::jthread m_fftThread;

    DragonPipe<DragonFftBlock> *m_fftPipe = nullptr;

    DragonPlayer::FftMode m_currentMode = DragonPlayer::FftMode::Off;
    int m_fftRate = 60;
    bool m_infrastructureCreated = false;

    FrameCallback m_frameCallback;
};
