/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

// A backend-free AudioSink/AudioSinkStream pair for driving PlaybackPipeline
// and Voice deterministically on the test thread: streams render when the
// test calls pump(), stream creation can be failed on demand, and every
// control (gain/mute/pause/name/clear) is recorded for fan-out assertions.
//
// Streams are QObjects; the registry keeps QPointers so assertions can also
// observe streams that were closed (and destroyed) by the pipeline: a null
// QPointer means "closed and gone".

#include "sink/audiosink.h"

#include <QPointer>

#include <chrono>
#include <expected>
#include <memory>
#include <span>
#include <vector>

class FakeAudioSinkStream;

class FakeAudioSink : public AudioSink
{
public:
    explicit FakeAudioSink(QObject *parent = nullptr)
        : AudioSink(parent)
    {
    }

    [[nodiscard]] bool probe() override
    {
        return true;
    }

    [[nodiscard]] std::expected<std::unique_ptr<AudioSinkStream>, QString>
    createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer) override;

    void setStreamName(const QString &name) override;

    // Streams registered with the backend and not closed yet.
    [[nodiscard]] const std::vector<FakeAudioSinkStream *> &liveStreams() const
    {
        return m_liveStreams;
    }
    // Every stream ever created (null QPointer = closed and destroyed).
    [[nodiscard]] const std::vector<QPointer<FakeAudioSinkStream>> &createdStreams() const
    {
        return m_createdStreams;
    }
    [[nodiscard]] int createAttempts() const
    {
        return m_createAttempts;
    }
    // Fail every createStream() call after the first N successful ones
    // (N < 0 disables injection, the default).
    void failStreamsAfter(int successfulCreations)
    {
        m_failStreamsFrom = successfulCreations;
    }

    void streamRegistered(FakeAudioSinkStream *stream)
    {
        m_liveStreams.push_back(stream);
    }
    void streamUnregistered(FakeAudioSinkStream *stream)
    {
        std::erase(m_liveStreams, stream);
    }

private:
    friend class FakeAudioSinkStream;

    std::vector<QPointer<FakeAudioSinkStream>> m_createdStreams;
    std::vector<FakeAudioSinkStream *> m_liveStreams;
    int m_createAttempts = 0;
    int m_failStreamsFrom = -1;
};

class FakeAudioSinkStream : public AudioSinkStream
{
public:
    FakeAudioSinkStream(FakeAudioSink &sink, int sampleRate, int channels, AudioSinkStreamRenderer &renderer)
        : AudioSinkStream(renderer)
        , sampleRate(sampleRate)
        , channels(channels)
        , m_sink(sink)
    {
        sink.streamRegistered(this);
    }

    ~FakeAudioSinkStream() override
    {
        std::ignore = close();
    }

    // What the test cares about: recorded control calls.
    int pauseCalls = 0;
    int resumeCalls = 0;
    int clearCalls = 0;
    QString recordedName;
    QList<float> gainHistory;
    QList<bool> muteHistory;
    const int sampleRate;
    const int channels;
    bool closed = false;

    void pause() override
    {
        ++pauseCalls;
        m_paused = true;
    }
    void resume() override
    {
        ++resumeCalls;
        m_paused = false;
    }
    [[nodiscard]] bool isPaused() const override
    {
        return m_paused;
    }
    void setGain(float linearGain) override
    {
        gainHistory.append(linearGain);
        lastGain = linearGain;
    }
    void setMuted(bool muted) override
    {
        muteHistory.append(muted);
    }
    void clearStream() override
    {
        ++clearCalls;
    }
    [[nodiscard]] qint64 deviceQueuedSamples() const override
    {
        return queuedSamples;
    }
    [[nodiscard]] int audioBufferFrames() const override
    {
        return bufferFrames;
    }
    [[nodiscard]] std::optional<std::chrono::microseconds> audioBufferDuration() const override
    {
        return std::chrono::microseconds{bufferFrames * 1000000 / static_cast<qint64>(sampleRate)};
    }

    // Drive one render quantum through the voice behind this stream, like a
    // backend audio callback would.
    std::span<const float> pump(std::size_t maxSamples, std::chrono::microseconds pts = {})
    {
        if (m_paused) {
            return {};
        }
        return renderer().render(maxSamples, pts);
    }

    void emitDrained()
    {
        Q_EMIT drained();
    }
    void emitError(const QString &message)
    {
        Q_EMIT errorOccurred(message);
    }
    void emitExternalVolumeChanged(float gain)
    {
        Q_EMIT externalVolumeChanged(gain);
    }
    void emitExternalVolumeRestored(float gain)
    {
        Q_EMIT externalVolumeRestored(gain);
    }
    void emitExternalMutedChanged(bool muted)
    {
        Q_EMIT externalMutedChanged(muted);
    }

    qint64 queuedSamples = 0;
    int bufferFrames = 0;
    float lastGain = 1.0f;

private:
    [[nodiscard]] bool closeStream() override
    {
        closed = true;
        m_sink.streamUnregistered(this);
        return true;
    }

    FakeAudioSink &m_sink;
    bool m_paused = false;
};

inline std::expected<std::unique_ptr<AudioSinkStream>, QString> FakeAudioSink::createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer)
{
    ++m_createAttempts;
    if (m_failStreamsFrom >= 0 && m_createAttempts > m_failStreamsFrom) {
        return std::unexpected(QStringLiteral("injected stream creation failure"));
    }
    auto stream = std::make_unique<FakeAudioSinkStream>(*this, sampleRate, channels, renderer);
    m_createdStreams.push_back(stream.get());
    return stream;
}

inline void FakeAudioSink::setStreamName(const QString &name)
{
    AudioSink::setStreamName(name);
    for (auto *stream : std::as_const(m_liveStreams)) {
        stream->recordedName = name;
    }
}
