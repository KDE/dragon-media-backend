/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"
#include <stdfloat>

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

class DRAGONSDL_EXPORT DragonAudioSink : public QObject
{
    Q_OBJECT

public:
    explicit DragonAudioSink(QObject *parent = nullptr);
    ~DragonAudioSink() override;

    DragonAudioSink(const DragonAudioSink &) = delete;
    DragonAudioSink &operator=(const DragonAudioSink &) = delete;
    DragonAudioSink(DragonAudioSink &&) = delete;
    DragonAudioSink &operator=(DragonAudioSink &&) = delete;

    // --- SPI (implemented by each backend) ---
    virtual void open(int sampleRate, int channels) = 0;
    virtual void close() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void setGain(float linearGain) = 0;
    [[nodiscard]] virtual int64_t deviceQueuedSamples() const = 0;

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

    void setPositionOffset(int64_t offsetMs, PositionResetMode mode = PositionResetMode::NormalTrackChange);

    [[nodiscard]] float volume() const;
    void setVolume(float volume);
    [[nodiscard]] bool muted() const;
    void setMuted(bool muted);

    virtual void setStreamName(const QString &name);
    [[nodiscard]] bool hasFormat(int sampleRate, int channels) const;
    [[nodiscard]] int64_t totalSamplesWritten() const;
    void resetPositionTracking();

    [[nodiscard]] int64_t positionMs() const;

    virtual void clearStream() = 0;

    void silence();
    void restoreVolume();
    void reset()
    {
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

Q_SIGNALS:
    void errorOccurred(const QString &message);
    void volumeChanged();

protected:
    std::span<const std::float32_t> processAudioCallback(size_t maxSamples, std::chrono::microseconds estimatedPts);

    int currentSampleRate() const
    {
        return m_sampleRate;
    }
    int currentChannels() const
    {
        return m_channels;
    }
    void setFormat(int sampleRate, int channels);

private:
    DragonPipe<std::float32_t> *audioPipe() const;
    DragonPipe<DragonFftBlock> *fftPipe() const;

    std::atomic<DragonPipe<std::float32_t> *> m_audioPipe{nullptr};
    std::atomic<DragonPipe<DragonFftBlock> *> m_fftPipe{nullptr};

    std::atomic<int64_t> m_positionOffsetMs{0};

    float m_volume = 1.0f;
    bool m_muted = false;

    int m_sampleRate = 0;
    int m_channels = 0;

    std::vector<std::float32_t> m_callbackBuffer;

    std::atomic<int64_t> m_totalSamplesWritten{0};
    std::atomic<bool> m_flushPending{false};
    std::atomic<bool> m_positionResetPending{false};
    std::atomic<bool> m_queueReady{true};
};
