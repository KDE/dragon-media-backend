/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmultimedia_export.h"
#include "dragonstdfloat_compat.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

template<typename T>
class DragonPipe;
struct DragonFftBlock;

class DRAGONMULTIMEDIA_EXPORT DragonAudioSink : public QObject
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
    [[nodiscard]] virtual int audioBufferUs() const
    {
        return -1;
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
    void setAudioPipe(DragonPipe<std::float32_t> *pipe);
    void setFftPipe(DragonPipe<DragonFftBlock> *pipe);

    void setQueueReady(bool ready);
    [[nodiscard]] bool isQueueReady() const;

    enum class PositionResetMode {
        Seek,
        GaplessTransition,
        NormalTrackChange
    };

    void setPositionOffset(qint64 offsetMs, PositionResetMode mode = PositionResetMode::NormalTrackChange);

    [[nodiscard]] float volume() const;
    void setVolume(float volume);
    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);

    virtual void setStreamName(const QString &name);
    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;
    [[nodiscard]] qint64 totalSamplesWritten() const;
    void resetPositionTracking();

    [[nodiscard]] qint64 positionMs() const;

    virtual void clearStream() = 0;

    void reset()
    {
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

    virtual void notifyDecodeFinished();
    virtual void resetDrainState();

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();
    void drained();

protected Q_SLOTS:
    void onExternalVolumeChanged(float linearGain);

protected:
    std::span<const std::float32_t> processAudioCallback(size_t maxSamples, std::chrono::microseconds estimatedPts);

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

private:
    DragonPipe<std::float32_t> *audioPipe() const;
    DragonPipe<DragonFftBlock> *fftPipe() const;

    std::atomic<DragonPipe<std::float32_t> *> m_audioPipe{nullptr};
    std::atomic<DragonPipe<DragonFftBlock> *> m_fftPipe{nullptr};

    std::atomic<qint64> m_positionOffsetMs{0};

    float m_volume = 1.0f;
    bool m_muted = false;

    std::atomic<int> m_sampleRate{0};
    std::atomic<int> m_channels{0};

    std::vector<std::float32_t> m_callbackBuffer;

    std::atomic<qint64> m_totalSamplesWritten{0};
    std::atomic<bool> m_flushPending{false};
    std::atomic<bool> m_positionResetPending{false};
    std::atomic<bool> m_queueReady{true};

protected:
    std::atomic<bool> m_decodeFinished{false};
    std::atomic<int> m_underrunCount{0};
};
