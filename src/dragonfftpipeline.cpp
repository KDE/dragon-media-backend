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

class DragonFftPipeline::Impl
{
public:
    static constexpr size_t kBufferCapacity = 65536;

    explicit Impl() = default;
    ~Impl() = default;

    std::unique_ptr<DragonFftProcessor> fftProcessor;

    std::jthread fftThread;

    LockFreeSpscQueue<std::float32_t> *fftQueue = nullptr;
    std::vector<std::float32_t> *fftBuffer = nullptr;
    std::condition_variable *waitCv = nullptr;

    DragonPlayer::FftMode currentMode = DragonPlayer::FftMode::Off;
    bool infrastructureCreated = false;

    FrameCallback frameCallback;

    void ensureInfrastructureInternal()
    {
        if (infrastructureCreated) {
            return;
        }

        if (fftBuffer && fftBuffer->empty()) {
            fftBuffer->resize(kBufferCapacity);
        }

        fftProcessor = std::make_unique<DragonFftProcessor>();
        fftProcessor->setWaitCv(waitCv);
        fftProcessor->setFftMode(currentMode);

        if (frameCallback) {
            fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
                frameCallback(std::move(frame));
            });
        }

        infrastructureCreated = true;

        qCDebug(dragonsdlFft) << "FFT infrastructure ensured";
    }

    void teardownInternal()
    {
        stopThread();
        fftProcessor.reset();
        if (fftBuffer) {
            fftBuffer->clear();
            fftBuffer->shrink_to_fit();
        }
        infrastructureCreated = false;
        fftQueue = nullptr;
        waitCv = nullptr;

        qCDebug(dragonsdlFft) << "FFT infrastructure torn down";
    }

    void startThread()
    {
        if (fftThread.joinable()) {
            return;
        }

        if (!fftProcessor) {
            qCWarning(dragonsdlFft) << "Cannot start FFT thread: no processor";
            return;
        }

        fftThread = std::jthread([this](std::stop_token st) {
            setCurrentThreadName("dragon-fft");
            fftProcessor->processLoop(std::move(st));
        });

        qCDebug(dragonsdlFft) << "FFT thread started";
    }

    void stopThread()
    {
        if (fftThread.joinable()) {
            fftThread.request_stop();
            fftThread.join();
            qCDebug(dragonsdlFft) << "FFT thread stopped";
        }
    }

    void setModeInternal(DragonPlayer::FftMode mode)
    {
        const bool wasOn = (currentMode != DragonPlayer::FftMode::Off);
        const bool nowOn = (mode != DragonPlayer::FftMode::Off);
        currentMode = mode;

        if (!wasOn && nowOn) {
            ensureInfrastructureInternal();

            if (fftProcessor) {
                fftProcessor->setFftMode(mode);
            }

            startThread();

            qCDebug(dragonsdlFft) << "FFT mode: Off -> On (" << static_cast<int>(mode) << ")";
        } else if (wasOn && !nowOn) {
            if (fftProcessor) {
                fftProcessor->setFftMode(mode);
            }

            qCDebug(dragonsdlFft) << "FFT mode: On -> Off";
        } else if (nowOn && fftProcessor) {
            fftProcessor->setFftMode(mode);

            qCDebug(dragonsdlFft) << "FFT mode change: " << static_cast<int>(mode);
        }
    }

    void restartWithNewQueueInternal(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv)
    {
        fftQueue = queue;
        waitCv = cv;

        stopThread();

        if (fftProcessor) {
            fftProcessor->setQueue(queue);
            fftProcessor->setWaitCv(cv);
            fftProcessor->reset();
        }

        startThread();

        qCDebug(dragonsdlFft) << "FFT restarted with new queue";
    }
};

DragonFftPipeline::DragonFftPipeline()
    : d(std::make_unique<Impl>())
{
}

DragonFftPipeline::~DragonFftPipeline() = default;

void DragonFftPipeline::ensureInfrastructure(std::vector<std::float32_t> *buffer, std::condition_variable *waitCv, DragonPlayer::FftMode mode)
{
    d->fftBuffer = buffer;
    d->waitCv = waitCv;
    d->currentMode = mode;

    if (!d->infrastructureCreated) {
        d->ensureInfrastructureInternal();
    } else if (d->fftProcessor) {
        d->fftProcessor->setFftMode(mode);
    }
}

void DragonFftPipeline::teardown()
{
    d->teardownInternal();
}

void DragonFftPipeline::setQueue(LockFreeSpscQueue<std::float32_t> *queue)
{
    d->fftQueue = queue;
    if (d->fftProcessor) {
        d->fftProcessor->setQueue(queue);
    }
}

void DragonFftPipeline::setWaitCv(std::condition_variable *cv)
{
    d->waitCv = cv;
    if (d->fftProcessor) {
        d->fftProcessor->setWaitCv(cv);
    }
}

void DragonFftPipeline::setSampleRate(int sampleRate)
{
    if (d->fftProcessor) {
        d->fftProcessor->setSampleRate(sampleRate);
    }
}

void DragonFftPipeline::setMode(DragonPlayer::FftMode mode)
{
    d->setModeInternal(mode);
}

DragonPlayer::FftMode DragonFftPipeline::mode() const
{
    return d->currentMode;
}

void DragonFftPipeline::start()
{
    d->startThread();
}

void DragonFftPipeline::stop()
{
    d->stopThread();
}

bool DragonFftPipeline::isRunning() const
{
    return d->fftThread.joinable();
}

void DragonFftPipeline::restartWithNewQueue(LockFreeSpscQueue<std::float32_t> *queue, std::condition_variable *cv)
{
    d->restartWithNewQueueInternal(queue, cv);
}

void DragonFftPipeline::setFrameCallback(FrameCallback cb)
{
    d->frameCallback = std::move(cb);

    if (d->fftProcessor) {
        d->fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            if (d->frameCallback) {
                d->frameCallback(std::move(frame));
            }
        });
    }
}

bool DragonFftPipeline::hasInfrastructure() const
{
    return d->infrastructureCreated;
}
