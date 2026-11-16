/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "playbackpipeline.h"
#include "audiosink.h"
#include "dragonmediabackend_audio_logging.h"
#include "nullaudiosink.h"

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
    , m_sink(std::move(sink))
{
    if (!m_sink) {
        m_sink = std::make_unique<NullAudioSink>();
    }
    connect(m_sink.get(), &AudioSink::errorOccurred, this, &PlaybackPipeline::errorOccurred);
}

PlaybackPipeline::~PlaybackPipeline() = default;

std::unique_ptr<Voice> PlaybackPipeline::makeVoice(Pipe<float> *pipe)
{
    const std::uint64_t generation = m_nextVoiceGeneration++;

    Voice::Notifiers notifiers;
    notifiers.trackStarted = [this, generation](std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, trackId, duration]() {
                if (generation == m_primaryGeneration) {
                    Q_EMIT trackStarted(trackId, duration);
                }
            },
            Qt::QueuedConnection);
    };
    notifiers.formatChangeRequired = [this, generation](int sampleRate, int channels) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, sampleRate, channels]() {
                if (generation == m_primaryGeneration) {
                    Q_EMIT formatChangeRequired(sampleRate, channels);
                }
            },
            Qt::QueuedConnection);
    };
    notifiers.fadingFinished = [this, generation, pipe]() {
        QMetaObject::invokeMethod(
            this,
            [this, generation, pipe]() {
                if (generation == m_fadingGeneration) {
                    onFadingVoiceEnded(pipe, QString());
                }
            },
            Qt::QueuedConnection);
    };

    return std::make_unique<Voice>(pipe, generation, std::move(notifiers));
}

void PlaybackPipeline::connectStreamSignals(AudioSinkStream *stream, std::uint64_t generation, Pipe<float> *pipe, bool firstStream)
{
    // Generation checks instead of signal disconnection: a queued event can
    // still arrive after the voice it belongs to was replaced.
    connect(stream, &AudioSinkStream::errorOccurred, this, [this, generation, pipe](const QString &message) {
        if (generation == m_fadingGeneration) {
            // A fading-stream error is not player-fatal, but its decoder must
            // not remain blocked: destroy the voice and notify by pipe identity.
            onFadingVoiceEnded(pipe, message);
        } else if (generation == m_primaryGeneration) {
            Q_EMIT errorOccurred(message);
        }
    });
    connect(stream, &AudioSinkStream::drained, this, [this, generation]() {
        if (generation == m_primaryGeneration) {
            Q_EMIT drained();
        }
    });
    connect(stream, &AudioSinkStream::externalVolumeChanged, this, [this, generation](float linearGain) {
        if (generation == m_primaryGeneration) {
            onExternalVolumeChanged(linearGain);
        }
    });
    connect(stream, &AudioSinkStream::externalVolumeRestored, this, [this, generation, firstStream](float linearGain) {
        // A startup restored volume is adopted only for the pipeline's first
        // primary stream; replacement/crossfade streams must not re-restore
        // an old per-node value over the user's current volume.
        if (firstStream && generation == m_primaryGeneration) {
            onExternalVolumeRestored(linearGain);
        }
    });
    connect(stream, &AudioSinkStream::externalMutedChanged, this, [this, generation](bool muted) {
        if (generation == m_primaryGeneration) {
            setMuted(muted);
        }
    });
}

void PlaybackPipeline::open(int sampleRate, int channels)
{
    if (!m_primary || sampleRate <= 0 || channels <= 0) {
        return;
    }

    // Tear down any existing stream first so the format change and pre-stream
    // setup below run with the audio callback quiesced (createStream() starts
    // the audio callback thread immediately).
    m_primary->closeStream();
    m_primary->setFormat(sampleRate, channels);
    m_primary->preAllocate(m_sink->preferredCallbackSamples(sampleRate, channels));
    m_primary->configureFadeIn(m_primary->pendingFadeIn());

    const bool firstStream = m_firstStream;
    auto created = m_sink->createStream(sampleRate, channels, *m_primary);
    if (!created) {
        const QString message = created.error();
        if (m_fading) {
            // Keep the failure local: the player aborts the fading voice and
            // retries the primary stream as the only remaining stream.
            qCWarning(dragonMediaBackendAudio) << "primary stream creation failed during a crossfade, retrying solo:" << message;
            return;
        }
        qCWarning(dragonMediaBackendAudio) << "primary stream creation failed:" << message;
        Q_EMIT errorOccurred(message);
        return;
    }
    std::unique_ptr<AudioSinkStream> stream = std::move(created.value());

    connectStreamSignals(stream.get(), m_primary->generation(), m_primary->pipe(), firstStream);
    m_firstStream = false;

    if (m_muted) {
        stream->setMuted(true);
    }
    if (m_gainSetByPipeline) {
        stream->setGain(sliderToLinearGain(m_volume));
    }

    m_primary->setStream(std::move(stream));
}

void PlaybackPipeline::pause()
{
    if (m_primary && m_primary->stream()) {
        m_primary->stream()->pause();
    }
    if (m_fading && m_fading->stream()) {
        m_fading->stream()->pause();
    }
}

void PlaybackPipeline::resume()
{
    if (m_primary && m_primary->stream()) {
        m_primary->stream()->resume();
    }
    if (m_fading && m_fading->stream()) {
        m_fading->stream()->resume();
    }
}

void PlaybackPipeline::clearStream()
{
    if (m_primary && m_primary->stream()) {
        m_primary->stream()->clearStream();
    }
}

qint64 PlaybackPipeline::deviceQueuedSamples() const
{
    return (m_primary && m_primary->stream()) ? m_primary->stream()->deviceQueuedSamples() : 0;
}

bool PlaybackPipeline::isPaused() const
{
    return m_primary && m_primary->stream() && m_primary->stream()->isPaused();
}

std::optional<std::chrono::microseconds> PlaybackPipeline::audioBufferDuration() const
{
    return (m_primary && m_primary->stream()) ? m_primary->stream()->audioBufferDuration() : std::nullopt;
}

int PlaybackPipeline::audioBufferFrames() const
{
    return (m_primary && m_primary->stream()) ? m_primary->stream()->audioBufferFrames() : -1;
}

void PlaybackPipeline::setAudioPipe(Pipe<float> *pipe)
{
    Q_ASSERT(!m_primary || !m_primary->stream());
    destroyFadingVoice();

    Pipe<PcmBlock> *const fftPipe = m_fftPipe;
    if (m_primary) {
        m_fftPipe = nullptr;
    }

    auto voice = makeVoice(pipe);
    std::ignore = voice->publishInitialStream(std::chrono::milliseconds{0});
    if (fftPipe) {
        voice->setFftPipe(fftPipe);
        m_fftPipe = fftPipe;
    }

    std::unique_ptr<Voice> oldPrimary;
    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        oldPrimary = std::move(m_primary);
        m_primaryGeneration = voice->generation();
        m_primary = std::move(voice);
    }
    dropVoice(std::move(oldPrimary));
}

void PlaybackPipeline::setFftPipe(Pipe<PcmBlock> *pipe)
{
    m_fftPipe = pipe;
    if (m_primary) {
        m_primary->setFftPipe(pipe);
    }
}

std::uint64_t PlaybackPipeline::setPositionOffset(std::chrono::milliseconds offset, Pipe<float> *primaryPipe, std::chrono::milliseconds fadeInDuration)
{
    if (!m_primary) {
        return 0;
    }

    // Any seek or hard switch abandons an active crossfade so its stream is
    // quiesced and its pipe is no longer read before the caller reuses slots.
    destroyFadingVoice();

    // Hard switch: a different, non-null pipe. The player has already
    // stopped both old sessions; destroy both voices and construct a
    // replacement pending voice around the new pipe. Its stream is opened
    // once the current format is known.
    if (primaryPipe != nullptr && primaryPipe != m_primary->pipe()) {
        Pipe<PcmBlock> *const fftPipe = m_fftPipe;
        m_fftPipe = nullptr;

        auto voice = makeVoice(primaryPipe);
        voice->setPendingFadeIn(fadeInDuration);
        const auto epoch = voice->publishInitialStream(offset);
        if (fftPipe) {
            voice->setFftPipe(fftPipe);
            m_fftPipe = fftPipe;
        }

        std::unique_ptr<Voice> oldPrimary;
        {
            std::scoped_lock lock(m_primaryVoiceMutex);
            oldPrimary = std::move(m_primary);
            m_primaryGeneration = voice->generation();
            m_primary = std::move(voice);
        }
        dropVoice(std::move(oldPrimary));
        return epoch;
    }

    Voice &primary = *m_primary;
    primary.clock().setPositionOffset(offset);
    primary.setPendingFadeIn(fadeInDuration);

    if (primary.stream() == nullptr || primary.stream()->isPaused()) {
        // Flush synchronously with the callback quiesced: no deferred switch
        // can be armed with no consumer, so the gate can never spin closed
        // waiting for a callback that will not run.
        std::ignore = primary.closeStream();
        return primary.runFlushQuiesced(fadeInDuration);
    }

    // Deferred flush: consumed by the next render callback. The epoch is
    // minted at staging, and the producers stay gated until the flush
    // publishes the new stream under it.
    return primary.stageFlush(fadeInDuration);
}

std::uint64_t PlaybackPipeline::armCrossfade(Pipe<float> *fadingPipe,
                                             Pipe<float> *newPrimaryPipe,
                                             std::chrono::milliseconds fadeOutDuration,
                                             std::chrono::milliseconds fadeInDuration)
{
    if (m_primary == nullptr || newPrimaryPipe == nullptr || m_primary->pipe() == newPrimaryPipe
        || (fadingPipe != nullptr && fadingPipe != m_primary->pipe())) {
        // Degenerate request (or direct misuse): fall back to the
        // hard-switch path so the new primary pipe is still installed.
        return setPositionOffset(std::chrono::milliseconds{0}, newPrimaryPipe, fadeInDuration);
    }
    if (m_fading) {
        // Production never reaches this: the player hard-switches when a
        // crossfade is already active.
        destroyFadingVoice();
    }

    auto incoming = makeVoice(newPrimaryPipe);
    incoming->setPendingFadeIn(fadeInDuration);
    // Publish the initial clock state synchronously (quiesced initialization,
    // not an audio-thread latch): the gate is open afterwards, so the decoder
    // may fill the incoming pipe while loading completes. No backend stream
    // exists for the incoming voice yet.
    const auto epoch = incoming->publishInitialStream(std::chrono::milliseconds{0});

    m_primary->demoteToFading(incoming->crossfadeLink(), fadeOutDuration, fadeInDuration > std::chrono::milliseconds{0});

    // Move the single FFT attachment to the incoming primary under the
    // writer guard; the outgoing fade is not visualized.
    if (m_fftPipe) {
        m_primary->setFftPipe(nullptr);
        incoming->setFftPipe(m_fftPipe);
    }

    {
        std::scoped_lock lock(m_primaryVoiceMutex);
        m_fading = std::move(m_primary);
        m_fadingGeneration = m_fading->generation();
        m_primaryGeneration = incoming->generation();
        m_primary = std::move(incoming);
    }
    return epoch;
}

void PlaybackPipeline::reset()
{
    destroyFadingVoice();

    if (m_primary) {
        // Reset the primary voice in place (with its callback quiesced): the
        // full clock reset un-sticks a stale Claimed/Emitted EOS so the next
        // stream can arm end-of-stream again, while the voice keeps its pipe
        // and format and the clock preserves its producer epoch and sample
        // counters for diagnostics.
        std::ignore = m_primary->closeStream();
        m_primary->resetForTeardown();
    }
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
    m_gainSetByPipeline = true;
    applyGainToVoices();
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
    if (m_primary && m_primary->stream()) {
        m_primary->stream()->setMuted(muted);
    }
    if (m_fading && m_fading->stream()) {
        m_fading->stream()->setMuted(muted);
    }
    Q_EMIT mutedChanged(m_muted);
}

void PlaybackPipeline::applyGainToVoices()
{
    const float gain = sliderToLinearGain(m_volume);
    if (m_primary && m_primary->stream()) {
        m_primary->stream()->setGain(gain);
    }
    if (m_fading && m_fading->stream()) {
        m_fading->stream()->setGain(gain);
    }
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
    if (!m_primary) {
        return std::chrono::milliseconds{0};
    }
    const StreamClock &clock = m_primary->clock();
    const int channels = clock.currentChannels();
    const int sampleRate = clock.currentSampleRate();
    const auto offset = clock.positionOffset();
    if (channels <= 0 || sampleRate <= 0) {
        return offset;
    }

    // Until the audio callback thread has consumed the staged flush,
    // totalSamplesWritten still counts pre-seek samples already played out.
    // Report just the new offset in that window.
    if (clock.isSwitchArmed()) {
        return offset;
    }

    qint64 written = clock.totalSamplesWritten();

    const qint64 queued = deviceQueuedSamples();
    if (queued > 0) {
        written = std::max(qint64{0}, written - queued);
    }

    const qint64 frameCount = written / channels;
    return std::chrono::milliseconds{frameCount * 1000 / sampleRate} + offset;
}

void PlaybackPipeline::destroyFadingVoice()
{
    if (!m_fading) {
        return;
    }
    m_fadingGeneration = 0;
    dropVoice(std::move(m_fading));
}

void PlaybackPipeline::dropVoice(std::unique_ptr<Voice> voice)
{
    if (!voice) {
        return;
    }
    if (!voice->closeStream()) {
        // The backend leaked its native stream with a stuck callback that
        // still renders from this voice: leak the whole voice instead of
        // freeing the renderer underneath it.
        qCCritical(dragonMediaBackendAudio) << "audio callback did not quiesce; leaking the voice (zombie state) to avoid a use-after-free";
        std::ignore = voice.release();
        return;
    }
    // ~Voice detaches the FFT tap, closes the stream, and destroys the
    // channel and the clock in a valid order (the pipe owner drains the pipe).
    voice.reset();
}

void PlaybackPipeline::onFadingVoiceEnded(Pipe<float> *pipe, const QString &error)
{
    if (m_fading && m_fading->pipe() == pipe) {
        destroyFadingVoice();
    }
    if (m_fadingVoiceEndedCallback) {
        m_fadingVoiceEndedCallback(pipe, error);
    }
}
