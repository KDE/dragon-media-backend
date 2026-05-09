/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonfftpipeline.h"
#include "dragonfftprocessor.h"

#include <LockFreeSpscQueue.h>

#include "dragonsdl_fft_logging.h"
#include "dragonsdl_logging.h"

#include <QMetaObject>

#include <algorithm>
#include <pthread.h>
#include <stop_token>
#include <thread>

static inline void setCurrentThreadName(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

DragonFftPipeline::DragonFftPipeline() = default;

DragonFftPipeline::~DragonFftPipeline() = default;

void DragonFftPipeline::ensureInfrastructureInternal()
{
    if (m_infrastructureCreated) {
        return;
    }

    if (m_fftBuffer && m_fftBuffer->empty()) {
        m_fftBuffer->resize(kBufferCapacity);
    }

    m_fftProcessor = std::make_unique<DragonFftProcessor>();
    m_fftProcessor->setWaitCv(m_waitCv);
    m_fftProcessor->setFftMode(m_currentMode);

    if (m_frameCallback) {
        m_fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            m_frameCallback(std::move(frame));
        });
    }

    m_infrastructureCreated = true;

    qCDebug(dragonsdlFft) << "FFT infrastructure ensured";
}

void DragonFftPipeline::teardownInternal()
{
    stopThread();
    m_fftProcessor.reset();
    if (m_fftBuffer) {
        m_fftBuffer->clear();
        m_fftBuffer->shrink_to_fit();
    }
    m_infrastructureCreated = false;
    m_fftQueue = nullptr;
    m_waitCv = nullptr;

    qCDebug(dragonsdlFft) << "FFT infrastructure torn down";
}

void DragonFftPipeline::startThread()
{
    if (m_fftThread.joinable()) {
        return;
    }

    if (!m_fftProcessor) {
        qCWarning(dragonsdlFft) << "Cannot start FFT thread: no processor";
        return;
    }

    m_fftThread = std::jthread([this](std::stop_token st) {
        setCurrentThreadName("dragon-fft");
        m_fftProcessor->processLoop(std::move(st));
    });

    qCDebug(dragonsdlFft) << "FFT thread started";
}

void DragonFftPipeline::stopThread()
{
    if (m_fftThread.joinable()) {
        m_fftThread.request_stop();
        if (m_waitCv) {
            m_waitCv->notify_all();
        }
        m_fftThread.join();
        qCDebug(dragonsdlFft) << "FFT thread stopped";
    }
}

void DragonFftPipeline::setModeInternal(DragonPlayer::FftMode mode)
{
    const bool wasOn = (m_currentMode != DragonPlayer::FftMode::Off);
    const bool nowOn = (mode != DragonPlayer::FftMode::Off);
    m_currentMode = mode;

    if (!wasOn && nowOn) {
        ensureInfrastructureInternal();

        if (m_fftProcessor) {
            m_fftProcessor->setFftMode(mode);
        }

        startThread();

        qCDebug(dragonsdlFft) << "FFT mode: Off -> On (" << static_cast<int>(mode) << ")";
    } else if (wasOn && !nowOn) {
        if (m_fftProcessor) {
            m_fftProcessor->setFftMode(mode);
        }

        qCDebug(dragonsdlFft) << "FFT mode: On -> Off";
    } else if (nowOn && m_fftProcessor) {
        m_fftProcessor->setFftMode(mode);

        qCDebug(dragonsdlFft) << "FFT mode change: " << static_cast<int>(mode);
    }
}

void DragonFftPipeline::restartWithQueueInternal(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv)
{
    m_fftQueue = queue;
    m_waitCv = cv;

    stopThread();

    if (m_fftProcessor) {
        m_fftProcessor->setQueue(queue);
        m_fftProcessor->setWaitCv(cv);
        m_fftProcessor->reset();
    }

    startThread();

    qCDebug(dragonsdlFft) << "FFT restarted with queue";
}

void DragonFftPipeline::ensureInfrastructure(std::vector<std::float32_t> *buffer, std::condition_variable *waitCv, DragonPlayer::FftMode mode)
{
    m_fftBuffer = buffer;
    m_waitCv = waitCv;
    m_currentMode = mode;

    if (!m_infrastructureCreated) {
        ensureInfrastructureInternal();
    } else if (m_fftProcessor) {
        m_fftProcessor->setFftMode(mode);
    }
}

void DragonFftPipeline::teardown()
{
    teardownInternal();
}

void DragonFftPipeline::setQueue(LockFreeSpscQueue<std::float32_t> *queue)
{
    m_fftQueue = queue;
    if (m_fftProcessor) {
        m_fftProcessor->setQueue(queue);
    }
}

void DragonFftPipeline::setWaitCv(std::condition_variable *cv)
{
    m_waitCv = cv;
    if (m_fftProcessor) {
        m_fftProcessor->setWaitCv(cv);
    }
}

void DragonFftPipeline::setSampleRate(int sampleRate)
{
    if (m_fftProcessor) {
        m_fftProcessor->setSampleRate(sampleRate);
    }
}

void DragonFftPipeline::setMode(DragonPlayer::FftMode mode)
{
    setModeInternal(mode);
}

DragonPlayer::FftMode DragonFftPipeline::mode() const
{
    return m_currentMode;
}

void DragonFftPipeline::start()
{
    startThread();
}

void DragonFftPipeline::stop()
{
    stopThread();
}

bool DragonFftPipeline::isRunning() const
{
    return m_fftThread.joinable();
}

void DragonFftPipeline::restartWithQueue(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv)
{
    restartWithQueueInternal(queue, cv);
}

void DragonFftPipeline::setFrameCallback(FrameCallback cb)
{
    m_frameCallback = std::move(cb);

    if (m_fftProcessor) {
        m_fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            if (m_frameCallback) {
                m_frameCallback(std::move(frame));
            }
        });
    }
}

bool DragonFftPipeline::hasInfrastructure() const
{
    return m_infrastructureCreated;
}
