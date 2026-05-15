/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonfftpipeline.h"
#include "dragonfftprocessor.h"

#include "dragonsdl_fft_logging.h"
#include "dragonsdl_logging.h"

#include <QMetaObject>

#include <algorithm>
#include <pthread.h>
#include <stop_token>
#include <thread>

DragonFftPipeline::DragonFftPipeline() = default;

DragonFftPipeline::~DragonFftPipeline() = default;

void DragonFftPipeline::ensureInfrastructure()
{
    if (m_infrastructureCreated) {
        return;
    }

    m_fftProcessor = std::make_unique<DragonFftProcessor>();
    m_fftProcessor->setFftMode(m_currentMode);

    if (m_fftPipe) {
        m_fftProcessor->setConsumer(m_fftPipe->consumer());
    }

    if (m_frameCallback) {
        m_fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            m_frameCallback(std::move(frame));
        });
    }

    m_infrastructureCreated = true;

    qCDebug(dragonsdlFft) << "FFT infrastructure ensured";
}

void DragonFftPipeline::teardown()
{
    stopThread();
    m_fftProcessor.reset();
    m_infrastructureCreated = false;
    m_fftPipe = nullptr;

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
        pthread_setname_np(pthread_self(), "dragon-fft");
        m_fftProcessor->processLoop(std::move(st));
    });

    qCDebug(dragonsdlFft) << "FFT thread started";
}

void DragonFftPipeline::stopThread()
{
    if (m_fftThread.joinable()) {
        m_fftThread.request_stop();
        if (m_fftPipe) {
            m_fftPipe->producer().notify();
        }
        m_fftThread.join();
        qCDebug(dragonsdlFft) << "FFT thread stopped";
    }
}

void DragonFftPipeline::restartThread()
{
    stopThread();
    if (m_fftProcessor) {
        m_fftProcessor->reset();
        m_fftProcessor->setFftMode(m_currentMode);
    }
    startThread();
}

void DragonFftPipeline::ensureInfrastructure(DragonPipe<std::float32_t> *pipe, DragonPlayer::FftMode mode)
{
    m_fftPipe = pipe;
    m_currentMode = mode;

    if (!m_infrastructureCreated) {
        ensureInfrastructure();
    } else if (m_fftProcessor) {
        m_fftProcessor->setFftMode(mode);
    }
}

void DragonFftPipeline::setSampleRate(int sampleRate)
{
    if (m_fftProcessor) {
        m_fftProcessor->setSampleRate(sampleRate);
    }
}

void DragonFftPipeline::setChannelCount(int channels)
{
    if (m_fftProcessor) {
        m_fftProcessor->setChannelCount(channels);
    }
}

void DragonFftPipeline::setMode(DragonPlayer::FftMode mode)
{
    const bool wasOn = (m_currentMode != DragonPlayer::FftMode::Off);
    const bool nowOn = (mode != DragonPlayer::FftMode::Off);
    m_currentMode = mode;

    if (!wasOn && nowOn) {
        ensureInfrastructure();

        if (m_fftProcessor) {
            m_fftProcessor->setFftMode(mode);
        }

        startThread();

        qCDebug(dragonsdlFft) << "FFT mode: Off -> On (" << mode << ")";
    } else if (wasOn && !nowOn) {
        if (m_fftProcessor) {
            m_fftProcessor->setFftMode(mode);
        }

        qCDebug(dragonsdlFft) << "FFT mode: On -> Off";
    } else if (nowOn && m_fftProcessor) {
        m_fftProcessor->setFftMode(mode);

        qCDebug(dragonsdlFft) << "FFT mode change: " << mode;
    }
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
