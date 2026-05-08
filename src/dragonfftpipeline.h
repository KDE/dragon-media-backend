/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonplayer.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

template<typename T>
class LockFreeSpscQueue;
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

    void ensureInfrastructure(std::vector<std::float32_t> *buffer, std::condition_variable *waitCv, DragonPlayer::FftMode mode);

    void teardown();
    void setQueue(LockFreeSpscQueue<std::float32_t> *queue);

    void setWaitCv(std::condition_variable *cv);

    void setSampleRate(int sampleRate);

    void setMode(DragonPlayer::FftMode mode);
    [[nodiscard]] DragonPlayer::FftMode mode() const;

    void start();
    void stop();
    [[nodiscard]] bool isRunning() const;

    void restartWithNewQueue(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv);

    using FrameCallback = std::function<void(DragonFftFrame)>;
    void setFrameCallback(FrameCallback cb);

    [[nodiscard]] bool hasInfrastructure() const;

private:
    static constexpr size_t kBufferCapacity = 65536;

    void ensureInfrastructureInternal();
    void teardownInternal();
    void startThread();
    void stopThread();
    void setModeInternal(DragonPlayer::FftMode mode);
    void restartWithNewQueueInternal(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv);

    std::unique_ptr<DragonFftProcessor> m_fftProcessor;

    std::jthread m_fftThread;

    LockFreeSpscQueue<std::float32_t> *m_fftQueue = nullptr;
    std::vector<std::float32_t> *m_fftBuffer = nullptr;
    std::condition_variable *m_waitCv = nullptr;

    DragonPlayer::FftMode m_currentMode = DragonPlayer::FftMode::Off;
    bool m_infrastructureCreated = false;

    FrameCallback m_frameCallback;
};
