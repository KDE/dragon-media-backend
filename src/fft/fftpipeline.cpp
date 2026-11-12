/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "fftpipeline.h"
#include "fftprocessor.h"

#include "dragonmediabackend_fft_logging.h"
#include "dragonmediabackend_logging.h"

#include <QMetaObject>

#include "threadname.h"
#include <algorithm>
#include <stop_token>
#include <thread>

FftPipeline::FftPipeline(Pipe<PcmBlock> *pipe)
    : m_fftPipe(pipe)
{
}

FftPipeline::~FftPipeline() = default;

void FftPipeline::ensureInfrastructure()
{
    if (m_infrastructureCreated) {
        return;
    }

    m_fftProcessor = std::make_unique<FftProcessor>();
    m_fftProcessor->setFftMode(m_currentMode);
    m_fftProcessor->setFftRate(m_fftRate);

    if (m_fftPipe) {
        m_fftProcessor->setConsumer(m_fftPipe->consumer());
    }

    if (m_frameCallback) {
        m_fftProcessor->setFrameCallback([this](DragonFftFrame frame) {
            m_frameCallback(std::move(frame));
        });
    }

    m_infrastructureCreated = true;

    qCDebug(dragonMediaBackendFft) << "FFT infrastructure ensured";
}

void FftPipeline::teardown()
{
    stopThread();
    m_fftProcessor.reset();
    m_infrastructureCreated = false;

    qCDebug(dragonMediaBackendFft) << "FFT infrastructure torn down";
}

void FftPipeline::startThread()
{
    if (m_fftThread.joinable()) {
        return;
    }

    if (!m_fftProcessor) {
        qCWarning(dragonMediaBackendFft) << "Cannot start FFT thread: no processor";
        return;
    }

    m_fftThread = std::jthread([this](const std::stop_token &st) {
        ThreadName::set("dragon-fft");
        m_fftProcessor->processLoop(st);
    });

    qCDebug(dragonMediaBackendFft) << "FFT thread started";
}

void FftPipeline::stopThread()
{
    if (m_fftThread.joinable()) {
        m_fftThread.request_stop();
        if (m_fftPipe) {
            m_fftPipe->producer().notify();
        }
        m_fftThread.join();
        qCDebug(dragonMediaBackendFft) << "FFT thread stopped";
    }
}

void FftPipeline::restartThread()
{
    stopThread();
    if (m_fftProcessor) {
        m_fftProcessor->reset();
        m_fftProcessor->setFftMode(m_currentMode);
    }
    startThread();
}

void FftPipeline::setSampleRate(int sampleRate)
{
    if (m_fftProcessor) {
        m_fftProcessor->setSampleRate(sampleRate);
    }
}

void FftPipeline::setChannelCount(int channels)
{
    if (m_fftProcessor) {
        m_fftProcessor->setChannelCount(channels);
    }
}

void FftPipeline::setMode(DragonSpectrumAnalyzer::Mode mode)
{
    const bool wasOn = (m_currentMode != DragonSpectrumAnalyzer::Mode::Off);
    const bool nowOn = (mode != DragonSpectrumAnalyzer::Mode::Off);
    m_currentMode = mode;

    if (!wasOn && nowOn) {
        ensureInfrastructure();

        if (m_fftProcessor) {
            m_fftProcessor->reset();
            m_fftProcessor->setFftMode(mode);
        }

        startThread();

        qCDebug(dragonMediaBackendFft) << "FFT mode: Off -> On (" << mode << ")";
    } else if (wasOn && !nowOn) {
        if (m_fftProcessor) {
            m_fftProcessor->setFftMode(mode);
        }

        qCDebug(dragonMediaBackendFft) << "FFT mode: On -> Off";
    } else if (nowOn && m_fftProcessor) {
        m_fftProcessor->setFftMode(mode);

        qCDebug(dragonMediaBackendFft) << "FFT mode change: " << mode;
    }
}

void FftPipeline::setFftRate(int rate)
{
    m_fftRate = rate;
    if (m_fftProcessor) {
        m_fftProcessor->setFftRate(rate);
    }
}

DragonSpectrumAnalyzer::Mode FftPipeline::mode() const
{
    return m_currentMode;
}

void FftPipeline::stop()
{
    stopThread();
}

void FftPipeline::restart()
{
    if (m_currentMode == DragonSpectrumAnalyzer::Mode::Off) {
        teardown();
        return;
    }
    ensureInfrastructure();
    restartThread();
}

void FftPipeline::setFrameCallback(FrameCallback cb)
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
