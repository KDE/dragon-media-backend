/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>
#include <QString>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

template<typename T>
class DragonPipe;
struct DragonPcmBlock;

class DRAGONMEDIABACKEND_EXPORT DragonAudioSink : public QObject
{
    Q_OBJECT

public:
    explicit DragonAudioSink(QObject *parent = nullptr);
    ~DragonAudioSink() override;

    DragonAudioSink(const DragonAudioSink &) = delete;
    DragonAudioSink &operator=(const DragonAudioSink &) = delete;
    DragonAudioSink(DragonAudioSink &&) = delete;
    DragonAudioSink &operator=(DragonAudioSink &&) = delete;

    // --- Runtime probe: call after construction to verify the backend can reach
    //     its audio subsystem (e.g. PipeWire socket, SDL audio device).
    //     Returns false if this sink will never be able to connect.
    //     The factory discards plugins that fail probe() and tries the next one.
    [[nodiscard]] virtual bool probe() = 0;

    // --- SPI (implemented by each backend) ---
    virtual void open(int sampleRate, int channels) = 0;
    virtual void close() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void setGain(float linearGain) = 0;
    [[nodiscard]] virtual qint64 deviceQueuedSamples() const = 0;

    [[nodiscard]] virtual bool isDeviceOpen() const = 0;
    [[nodiscard]] virtual bool isPaused() const = 0;

    // --- Diagnostics ---
    [[nodiscard]] virtual std::optional<std::chrono::microseconds> audioBufferDuration() const
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual int audioBufferFrames() const
    {
        return -1;
    }
    [[nodiscard]] int underrunCount() const
    {
        return m_underrunCount.load(std::memory_order_relaxed);
    }

    // --- Shared logic (in base class, not virtual) ---
    void setAudioPipe(DragonPipe<float> *pipe);
    void setFftPipe(DragonPipe<DragonPcmBlock> *pipe);

    void setQueueReady(bool ready);
    [[nodiscard]] bool isQueueReady() const;
    [[nodiscard]] bool isFlushPending() const
    {
        return m_flushPending.load(std::memory_order_acquire);
    }

    enum class PositionResetMode {
        Seek,
        NormalTrackChange
    };

    void setPositionOffset(std::chrono::milliseconds offset, PositionResetMode mode = PositionResetMode::NormalTrackChange);

    [[nodiscard]] float volume() const;
    void setVolume(float volume);
    [[nodiscard]] bool muted() const;
    virtual void setMuted(bool muted);

    virtual void setStreamName(const QString &name);
    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;
    [[nodiscard]] qint64 totalSamplesWritten() const;
    void resetPositionTracking();

    [[nodiscard]] qint64 streamSamplesRead() const
    {
        return static_cast<qint64>(m_streamSamplesRead.load(std::memory_order_relaxed));
    }

    [[nodiscard]] std::chrono::milliseconds position() const;

    virtual void clearStream() = 0;

    void reset()
    {
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

    struct StreamBoundary {
        std::uint64_t epoch = 0; // stream epoch the entry was recorded in
        std::uint64_t streamSample = 0; // absolute sample index the boundary applies after
        std::uint32_t trackId = 0;
        int sampleRate = 0;
        int channels = 0;
        std::optional<std::chrono::milliseconds> duration;
    };

    bool boundaryTrackStart(std::uint32_t trackId, int sampleRate, int channels, std::optional<std::chrono::milliseconds> duration);

    bool boundaryEndOfStream();

    bool retractEndOfStream();

    void samplesProduced(std::uint64_t epochTag, std::size_t count);

    [[nodiscard]] std::uint64_t streamEpoch() const
    {
        return m_boundaryEpoch.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool endOfStreamPending() const;

    [[nodiscard]] bool endOfStreamArmed() const
    {
        return m_eosState.load(std::memory_order_acquire) != static_cast<std::int8_t>(EosState::None);
    }

    [[nodiscard]] bool tryClaimDrained();

    [[nodiscard]] bool takeDrainedEmission();

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();
    void mutedChanged(bool muted);
    void drained();

    void trackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration);

    void formatChangeRequired(int sampleRate, int channels);

protected Q_SLOTS:
    void onExternalVolumeChanged(float linearGain);

protected:
    std::span<const float> processAudioCallback(size_t maxSamples, std::chrono::microseconds estimatedPts);

    int currentSampleRate() const
    {
        return m_sampleRate.load(std::memory_order_relaxed);
    }
    int currentChannels() const
    {
        return m_channels.load(std::memory_order_relaxed);
    }
    void setFormat(int sampleRate, int channels);

    void preAllocateCallbackBuffer(size_t maxSamples);

    [[nodiscard]] static QString defaultStreamName();
    [[nodiscard]] static QString applicationIconName();
    [[nodiscard]] QString resolvedStreamName() const;

    QString m_streamName;

private:
    DragonPipe<float> *audioPipe() const;
    DragonPipe<DragonPcmBlock> *fftPipe() const;

    class BoundaryRing
    {
    public:
        static constexpr std::size_t kCapacity = 8;

        bool push(const StreamBoundary &boundary)
        {
            const auto head = m_head.load(std::memory_order_relaxed);
            const auto tail = m_tail.load(std::memory_order_acquire);
            if (head - tail >= kCapacity) {
                return false;
            }
            m_storage[head & kMask] = boundary;
            m_head.store(head + 1, std::memory_order_release);
            return true;
        }

        [[nodiscard]] const StreamBoundary *peek() const
        {
            const auto tail = m_tail.load(std::memory_order_relaxed);
            if (m_head.load(std::memory_order_acquire) == tail) {
                return nullptr;
            }
            return &m_storage[tail & kMask];
        }

        void pop()
        {
            m_tail.fetch_add(1, std::memory_order_release);
        }

    private:
        static constexpr std::size_t kMask = kCapacity - 1;

        std::array<StreamBoundary, kCapacity> m_storage{};
        std::atomic<std::uint64_t> m_head{0};
        std::atomic<std::uint64_t> m_tail{0};
    };

    void processStreamFlush();

    std::atomic<DragonPipe<float> *> m_audioPipe{nullptr};
    std::atomic<DragonPipe<DragonPcmBlock> *> m_fftPipe{nullptr};

    std::atomic<std::chrono::milliseconds::rep> m_positionOffset{0};

    float m_volume = 1.0f;
    bool m_muted = false;

    std::atomic<int> m_sampleRate{0};
    std::atomic<int> m_channels{0};

    std::vector<float> m_callbackBuffer;

    std::atomic<qint64> m_totalSamplesWritten{0};
    std::atomic<bool> m_flushPending{false};
    std::atomic<bool> m_positionResetPending{false};
    std::atomic<bool> m_queueReady{true};

    BoundaryRing m_boundaries; // producer: decode thread; consumer: callback
    std::atomic<std::uint64_t> m_boundaryEpoch{1}; // bumped on flush
    std::atomic<std::uint64_t> m_producerSamples{0}; // decode thread (+ flush reset)
    std::atomic<std::uint64_t> m_streamSamplesRead{0}; // audio callback

    enum class EosState : std::int8_t {
        None,
        Pending,
        Claimed,
        Emitted,
    };
    std::atomic<std::int8_t> m_eosState{static_cast<std::int8_t>(EosState::None)};
    std::atomic<std::uint64_t> m_eosSample{0};
    std::atomic<bool> m_formatChangePending{false};

protected:
    std::atomic<int> m_underrunCount{0};
};
