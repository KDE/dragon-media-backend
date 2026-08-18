/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonfftpipeline.h"
#include "dragonfftprocessor.h"

#include "dragonmediabackend_fft_logging.h"
#include "dragonmediabackend_logging.h"

#include <QMetaObject>

#include "dragonthreadname.h"
#include <algorithm>
#include <stop_token>
#include <thread>

DragonFftPipeline::DragonFftPipeline(DragonPipe<DragonFftBlock> *pipe)
    : m_fftPipe(pipe)
{
}

DragonFftPipeline::~DragonFftPipeline() = default;

void DragonFftPipeline::ensureInfrastructure()
{
    if (m_infrastructureCreated) {
        return;
    }

    m_fftProcessor = std::make_unique<DragonFftProcessor>();
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

void DragonFftPipeline::teardown()
{
    stopThread();
    m_fftProcessor.reset();
    m_infrastructureCreated = false;

    qCDebug(dragonMediaBackendFft) << "FFT infrastructure torn down";
}

void DragonFftPipeline::startThread()
{
    if (m_fftThread.joinable()) {
        return;
    }

    if (!m_fftProcessor) {
        qCWarning(dragonMediaBackendFft) << "Cannot start FFT thread: no processor";
        return;
    }

    m_fftThread = std::jthread([this](std::stop_token st) {
        DragonThreadName::set("dragon-fft");
        m_fftProcessor->processLoop(std::move(st));
    });

    qCDebug(dragonMediaBackendFft) << "FFT thread started";
}

void DragonFftPipeline::stopThread()
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

void DragonFftPipeline::restartThread()
{
    stopThread();
    if (m_fftProcessor) {
        m_fftProcessor->reset();
        m_fftProcessor->setFftMode(m_currentMode);
    }
    startThread();
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

void DragonFftPipeline::setMode(DragonSpectrumAnalyzer::Mode mode)
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

void DragonFftPipeline::setFftRate(int rate)
{
    m_fftRate = rate;
    if (m_fftProcessor) {
        m_fftProcessor->setFftRate(rate);
    }
}

DragonSpectrumAnalyzer::Mode DragonFftPipeline::mode() const
{
    return m_currentMode;
}

void DragonFftPipeline::stop()
{
    stopThread();
}

void DragonFftPipeline::restart()
{
    if (m_currentMode == DragonSpectrumAnalyzer::Mode::Off) {
        teardown();
        return;
    }
    ensureInfrastructure();
    restartThread();
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
