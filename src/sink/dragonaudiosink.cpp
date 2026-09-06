/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonaudiosink.h"
#include "fft/dragonpcmblock.h"
#include "player/dragonpipe.h"

#include <QGuiApplication>
#include <QIcon>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace
{

float sliderToLinearGain(float sliderValue)
{
    sliderValue = std::clamp(sliderValue, 0.0f, 1.0f);

    if (sliderValue <= 0.001f) {
        return 0.0f;
    }

    if (sliderValue >= 0.99f) {
        return 1.0f;
    }

    constexpr float LOG100 = 4.60517018599f;
    return -std::log(1.0f - sliderValue) / LOG100;
}

float calculateGain(float volume, bool muted)
{
    return muted ? 0.0f : sliderToLinearGain(volume);
}

float linearGainToSlider(float linearGain)
{
    if (linearGain <= 0.0f) {
        return 0.0f;
    }
    if (linearGain >= 1.0f) {
        return 1.0f;
    }
    constexpr float LOG100 = 4.60517018599f;
    return 1.0f - std::exp(-linearGain * LOG100);
}
}

DragonAudioSink::DragonAudioSink(QObject *parent)
    : QObject(parent)
{
}

DragonAudioSink::~DragonAudioSink() = default;

void DragonAudioSink::setAudioPipe(DragonPipe<float> *pipe)
{
    m_audioPipe.store(pipe, std::memory_order_release);
}

void DragonAudioSink::setFftPipe(DragonPipe<DragonPcmBlock> *pipe)
{
    m_fftPipe.store(pipe, std::memory_order_release);
}

void DragonAudioSink::setQueueReady(bool ready)
{
    m_queueReady.store(ready, std::memory_order_release);
}

bool DragonAudioSink::isQueueReady() const
{
    return m_queueReady.load(std::memory_order_acquire);
}

void DragonAudioSink::setPositionOffset(qint64 offsetMs, PositionResetMode mode)
{
    m_positionOffsetMs.store(offsetMs, std::memory_order_relaxed);

    if (isPaused()) {
        if (mode == PositionResetMode::Seek || mode == PositionResetMode::NormalTrackChange) {
            auto *pipe = m_audioPipe.load(std::memory_order_acquire);
            if (pipe) {
                pipe->consumer().drain();
            }
            close();
            setQueueReady(true);
        }
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        return;
    }

    switch (mode) {
    case PositionResetMode::Seek:
    case PositionResetMode::NormalTrackChange:
        m_flushPending.store(true, std::memory_order_release);
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    case PositionResetMode::GaplessTransition:
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    }
}

float DragonAudioSink::volume() const
{
    return m_volume;
}

void DragonAudioSink::setVolume(float volume)
{
    float clampedVolume = std::clamp(volume, 0.0f, 1.0f);
    if (qAbs(m_volume - clampedVolume) < 0.001f) {
        return;
    }
    m_volume = clampedVolume;
    setGain(calculateGain(m_volume, m_muted));
    Q_EMIT volumeChanged();
}

bool DragonAudioSink::muted() const
{
    return m_muted;
}

void DragonAudioSink::setMuted(bool muted)
{
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    setGain(calculateGain(m_volume, m_muted));
    Q_EMIT mutedChanged(m_muted);
}

void DragonAudioSink::onExternalVolumeChanged(float linearGain)
{
    setVolume(linearGainToSlider(linearGain));
}

void DragonAudioSink::setStreamName(const QString &name)
{
    m_streamName = name;
}

QString DragonAudioSink::defaultStreamName()
{
    const QString appName = QGuiApplication::applicationDisplayName();
    return appName.isEmpty() ? QStringLiteral("DragonMediaBackend") : appName;
}

QString DragonAudioSink::resolvedStreamName() const
{
    return m_streamName.isEmpty() ? defaultStreamName() : m_streamName;
}

QString DragonAudioSink::applicationIconName()
{
    QString name = QGuiApplication::desktopFileName();
    if (name.isEmpty() && qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        name = QGuiApplication::windowIcon().name();
    }
    return name;
}

qint64 DragonAudioSink::positionMs() const
{
    int channels = m_channels.load(std::memory_order_relaxed);
    int sampleRate = m_sampleRate.load(std::memory_order_relaxed);
    if (channels <= 0 || sampleRate <= 0) {
        return m_positionOffsetMs.load(std::memory_order_relaxed);
    }

    qint64 written = m_totalSamplesWritten.load(std::memory_order_relaxed);

    const qint64 queued = deviceQueuedSamples();
    if (queued > 0) {
        written = std::max(qint64{0}, written - queued);
    }

    const qint64 frameCount = written / channels;
    return (frameCount * 1000 / sampleRate) + m_positionOffsetMs.load(std::memory_order_relaxed);
}

bool DragonAudioSink::hasFormat(int sampleRate, int channels) const
{
    return m_sampleRate.load(std::memory_order_relaxed) == sampleRate && m_channels.load(std::memory_order_relaxed) == channels;
}

qint64 DragonAudioSink::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

void DragonAudioSink::resetPositionTracking()
{
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    m_positionOffsetMs.store(0, std::memory_order_relaxed);
}

void DragonAudioSink::notifyDecodeFinished()
{
    m_drain.notifyDecodeFinished();
}

void DragonAudioSink::resetDrainState()
{
    m_drain.startNewEpoch();
}

std::span<const float> DragonAudioSink::processAudioCallback(size_t maxSamples, std::chrono::microseconds estimatedPts)
{
    if (m_positionResetPending.exchange(false, std::memory_order_acq_rel)) {
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

    if (m_flushPending.exchange(false, std::memory_order_acq_rel)) {
        if (auto *ap = m_audioPipe.load(std::memory_order_acquire)) {
            ap->consumer().drain();
        }
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        m_queueReady.store(true, std::memory_order_release);
        return {};
    }

    auto *ap = m_audioPipe.load(std::memory_order_acquire);
    if (!ap) {
        return {};
    }

    if (m_callbackBuffer.size() < maxSamples) {
        m_callbackBuffer.resize(maxSamples);
    }
    size_t totalRead = 0;

    ap->consumer().readSomeWith(maxSamples, [&](std::span<const float> b1, std::span<const float> b2) {
        totalRead = b1.size() + b2.size();
        if (totalRead == 0) {
            return;
        }

        auto it = m_callbackBuffer.begin();
        it = std::ranges::copy(b1, it).out;
        std::ranges::copy(b2, it);
    });

    if (totalRead == 0) {
        if (!m_drain.decodeFinished()) {
            m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        }
        return {};
    }

    const auto consumedSpan = std::span{m_callbackBuffer.data(), totalRead};

    if (auto *fp = m_fftPipe.load(std::memory_order_acquire)) {
        size_t blocksNeeded = (totalRead + DragonPcmBlock::MAX_SAMPLES - 1) / DragonPcmBlock::MAX_SAMPLES;
        fp->producer().writeSomeWith(blocksNeeded, [&](std::span<DragonPcmBlock> fb1, std::span<DragonPcmBlock> fb2) {
            size_t srcOffset = 0;
            auto fillBlock = [&](std::span<DragonPcmBlock> dst) {
                for (size_t i = 0; i < dst.size() && srcOffset < totalRead; ++i) {
                    size_t remaining = totalRead - srcOffset;
                    size_t toCopy = std::min(remaining, DragonPcmBlock::MAX_SAMPLES);
                    dst[i].count = toCopy;
                    dst[i].pts = estimatedPts;

                    for (size_t j = 0; j < toCopy; ++j) {
                        dst[i].samples[j] = m_callbackBuffer[srcOffset + j];
                    }
                    srcOffset += toCopy;
                }
            };
            fillBlock(fb1);
            fillBlock(fb2);
        });
    }

    m_totalSamplesWritten.fetch_add(static_cast<qint64>(totalRead), std::memory_order_relaxed);

    return consumedSpan;
}

DragonPipe<float> *DragonAudioSink::audioPipe() const
{
    return m_audioPipe.load(std::memory_order_acquire);
}

DragonPipe<DragonPcmBlock> *DragonAudioSink::fftPipe() const
{
    return m_fftPipe.load(std::memory_order_acquire);
}

void DragonAudioSink::setFormat(int sampleRate, int channels)
{
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
    m_channels.store(channels, std::memory_order_relaxed);
}

void DragonAudioSink::preAllocateCallbackBuffer(size_t maxSamples)
{
    m_callbackBuffer.resize(maxSamples);
}
