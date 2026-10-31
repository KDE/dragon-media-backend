/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "playbackpipeline.h"
#include "audiosink.h"
#include "dragonmediabackend_audio_logging.h"
#include "mixer/mixer.h"
#include "nullaudiosink.h"
#include "streamclock.h"

#include <QMetaObject>

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

PlaybackPipeline::PlaybackPipeline(std::unique_ptr<AudioSink> sink, QObject *parent)
    : QObject(parent)
    , m_mixer(m_clock)
    , m_sink(std::move(sink))
{
    if (!m_sink) {
        m_sink = std::make_unique<NullAudioSink>();
    }
    m_sink->setMixer(&m_mixer);

    StreamClock::EventHandlers handlers;
    handlers.trackStarted = [this](std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration) {
        QMetaObject::invokeMethod(
            this,
            [this, trackId, duration]() {
                Q_EMIT trackStarted(trackId, duration);
            },
            Qt::QueuedConnection);
    };
    handlers.formatChangeRequired = [this](int sampleRate, int channels) {
        QMetaObject::invokeMethod(
            this,
            [this, sampleRate, channels]() {
                Q_EMIT formatChangeRequired(sampleRate, channels);
            },
            Qt::QueuedConnection);
    };
    m_clock.setEventHandlers(std::move(handlers));

    // The audio thread notifies after consuming a staged switch request when
    // the main thread is waiting in waitForPendingCrossfade(). Both the claim
    // release on m_pendingSeq and this exchange are seq_cst, forbidding lost
    // wakeups without taking any mutex on the real-time callback thread.
    m_mixer.setSwitchAckedCallback([this]() {
        if (m_switchAckWaiting.exchange(false, std::memory_order_seq_cst)) {
            m_switchAckSemaphore.release();
        }
    });

    connect(m_sink.get(), &AudioSink::errorOccurred, this, &PlaybackPipeline::errorOccurred);
    connect(m_sink.get(), &AudioSink::drained, this, &PlaybackPipeline::drained);
    connect(m_sink.get(), &AudioSink::externalVolumeRestored, this, &PlaybackPipeline::onExternalVolumeRestored);
    connect(m_sink.get(), &AudioSink::externalVolumeChanged, this, &PlaybackPipeline::onExternalVolumeChanged);
    connect(m_sink.get(), &AudioSink::externalMutedChanged, this, &PlaybackPipeline::setMuted);
}

PlaybackPipeline::~PlaybackPipeline() = default;

void PlaybackPipeline::open(int sampleRate, int channels)
{
    if (sampleRate <= 0 || channels <= 0) {
        return;
    }
    // Tear down any existing session first so the mixer reset below runs with
    // the audio callback quiesced.
    if (m_sink->isDeviceOpen()) {
        m_sink->close();
    }

    m_clock.setFormat(sampleRate, channels);
    m_mixer.resetChannels();
    m_mixer.preAllocate(m_sink->preferredCallbackSamples(sampleRate, channels));

    m_sink->open(sampleRate, channels);
}

std::uint64_t PlaybackPipeline::setPositionOffset(std::chrono::milliseconds offset, Pipe<float> *primaryPipe, std::chrono::milliseconds fadeInDuration)
{
    m_clock.setPositionOffset(offset);

    if (!m_sink->isDeviceOpen() || m_sink->isPaused()) {
        // Flush synchronously with the callback quiesced: no deferred switch
        // can be armed with no consumer, so the gate can never spin closed
        // waiting for a callback that will not run.
        m_clock.armSwitch();
        if (m_sink->isDeviceOpen()) {
            m_sink->close();
        }
        if (primaryPipe) {
            m_mixer.setAudioPipe(primaryPipe);
        }
        return m_mixer.flushStreams(primaryPipe, fadeInDuration);
    }

    // Deferred flush: consumed by the next render callback. The epoch is
    // minted at staging, and the producers stay gated until the flush
    // publishes the new stream under it.
    return m_mixer.stageFlush(primaryPipe, fadeInDuration);
}

bool PlaybackPipeline::waitForPendingCrossfade(std::chrono::milliseconds timeout)
{
    if (!m_mixer.isCrossfadePending()) {
        return true;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_mixer.isCrossfadePending()) {
        while (m_switchAckSemaphore.try_acquire()) { }
        m_switchAckWaiting.store(true, std::memory_order_seq_cst);
        if (!m_mixer.isCrossfadePending()) {
            m_switchAckWaiting.store(false, std::memory_order_release);
            return true;
        }
        if (!m_switchAckSemaphore.try_acquire_until(deadline) && std::chrono::steady_clock::now() >= deadline) {
            m_switchAckWaiting.store(false, std::memory_order_release);
            return !m_mixer.isCrossfadePending();
        }
    }
    m_switchAckWaiting.store(false, std::memory_order_release);
    return true;
}

std::uint64_t PlaybackPipeline::armCrossfade(Pipe<float> *fadingPipe,
                                             Pipe<float> *newPrimaryPipe,
                                             std::chrono::milliseconds fadeOutDuration,
                                             std::chrono::milliseconds fadeInDuration)
{
    if (!waitForPendingCrossfade(std::chrono::milliseconds(150))) {
        qCWarning(dragonMediaBackendAudio) << "armCrossfade: previous switch not consumed within 150ms; superseding with a flush install";
        // Supersede the stuck request with a flush that installs the new
        // primary pipe on the audio thread: one switch path for both
        // outcomes, no dead-pipe fallback.
        return m_mixer.stageFlush(newPrimaryPipe, fadeInDuration);
    }
    if (const auto epoch = m_mixer.armCrossfade(fadingPipe, newPrimaryPipe, fadeOutDuration, fadeInDuration)) {
        return *epoch;
    }
    return m_mixer.stageFlush(newPrimaryPipe, fadeInDuration);
}

float PlaybackPipeline::volume() const
{
    return m_volume;
}

void PlaybackPipeline::setVolume(float volume)
{
    float clampedVolume = std::clamp(volume, 0.0f, 1.0f);
    if (qAbs(m_volume - clampedVolume) < 0.001f) {
        return;
    }
    m_volume = clampedVolume;
    m_sink->setGain(calculateGain(m_volume, m_muted));
    Q_EMIT volumeChanged();
}

bool PlaybackPipeline::muted() const
{
    return m_muted;
}

void PlaybackPipeline::setMuted(bool muted)
{
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    m_sink->setMuted(muted);
    m_sink->setGain(calculateGain(m_volume, m_muted));
    Q_EMIT mutedChanged(m_muted);
}

void PlaybackPipeline::onExternalVolumeChanged(float linearGain)
{
    setVolume(linearGainToSlider(linearGain));
}

void PlaybackPipeline::onExternalVolumeRestored(float linearGain)
{
    const float volume = linearGainToSlider(linearGain);
    qCDebug(dragonMediaBackendAudio) << "PlaybackPipeline: adopted server-restored volume:" << volume << "(linear gain:" << linearGain << ")";
    m_volume = std::clamp(volume, 0.0f, 1.0f);
}

std::chrono::milliseconds PlaybackPipeline::position() const
{
    int channels = m_clock.currentChannels();
    int sampleRate = m_clock.currentSampleRate();
    const auto offset = m_clock.positionOffset();
    if (channels <= 0 || sampleRate <= 0) {
        return offset;
    }

    // Until the audio callback thread has consumed the staged switch request,
    // m_totalSamplesWritten still counts pre-seek samples already played out.
    // Report just the new offset in that window.
    if (m_clock.isSwitchArmed() || m_mixer.isCrossfadePending()) {
        return offset;
    }

    qint64 written = m_clock.totalSamplesWritten();

    const qint64 queued = m_sink->deviceQueuedSamples();
    if (queued > 0) {
        written = std::max(qint64{0}, written - queued);
    }

    const qint64 frameCount = written / channels;
    return std::chrono::milliseconds{frameCount * 1000 / sampleRate} + offset;
}
