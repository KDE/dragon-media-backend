/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "decoder/decodepipeline.h"
#include "sink/playbackpipeline.h"
#include <dragonaudiooutput.h>
#include <dragonplayer.h>

#include "pipe.h"

#include <QObject>
#include <QTimer>
#include <QUrl>

#include <QCoroTask>
#include <QHash>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

using namespace std::chrono_literals;

class Diagnostics;
class DragonAudioOutput;
class PositionEstimator;

struct AliveGuard {
    std::atomic<bool> alive{true};
};

class DragonPlayerPrivate : public QObject
{
    Q_OBJECT
public:
    explicit DragonPlayerPrivate(DragonPlayer *player, DragonAudioOutput::Backend requestedBackend = DragonAudioOutput::Backend::Auto);
    Q_DISABLE_COPY_MOVE(DragonPlayerPrivate)

private:
    friend class Diagnostics;
    friend class DragonPlayer;
    friend class DragonSpectrumAnalyzer;
    friend class DragonSpectrumAnalyzerPrivate;

    void init();

    PlaybackPipeline *playbackPipeline() const;

    DragonPlayer *q = nullptr;

    DragonAudioOutput *audioOutput = nullptr;

private Q_SLOTS:
    void onDecodeFinished(const QUrl &source, bool hadFatalError);
    void onDecodeError(const QString &message);

    void onPlaybackTrackStarted(std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration);
    void onPlaybackFormatChangeRequired(int sampleRate, int channels);

    void onStreamStalled();
    void onStreamBuffering();
    void onStreamBuffered();
    void onStreamSeekable(bool seekable);

private:
    bool streamStateTransitionsAllowed() const;

    void applyRequestedState(int sampleRate, int channels, DragonPlayer::PlaybackState intent);
    void reportDeviceOpenFailure();

    void writeToQueues(std::span<const float> pcm, const std::stop_token &st);

    struct PipelineSlot {
        explicit PipelineSlot(DragonPlayer *parent)
            : decodePipeline(parent)
        {
        }
        DecodePipeline decodePipeline;
        Pipe<float> audioPipe{Pipe<float>::kDefaultCapacity};
        // Single content/session tag: bumped by invalidateSlotContent() and
        // invalidateQueuedContent() alike; an in-flight producer write whose
        // tag no longer matches aborts, and a demoted slot's writer stamps a
        // stale stream epoch the sink rejects.
        std::atomic<uint64_t> contentEpoch{0};
        std::atomic<uint64_t> streamEpoch{1};
    };

    // Two rotating (decode pipeline, audio pipe) slots. Crossfading needs the
    // outgoing track's tail and the incoming track's head decoded concurrently.
    // If a track change occurs while a crossfade is already running, the running
    // crossfade is aborted immediately, cutting the fading track, so a third
    // slot is never needed.
    std::array<PipelineSlot, 2> m_slots{PipelineSlot{q}, PipelineSlot{q}};
    std::atomic<std::uint8_t> m_primarySlotIndex{0};

    [[nodiscard]] PipelineSlot &primarySlot() noexcept
    {
        return m_slots[m_primarySlotIndex.load(std::memory_order_acquire) & 1u];
    }
    [[nodiscard]] const PipelineSlot &primarySlot() const noexcept
    {
        return m_slots[m_primarySlotIndex.load(std::memory_order_acquire) & 1u];
    }
    [[nodiscard]] PipelineSlot &fadingSlot() noexcept
    {
        return m_slots[(m_primarySlotIndex.load(std::memory_order_acquire) ^ 1u) & 1u];
    }
    [[nodiscard]] PipelineSlot &rotateSlots() noexcept
    {
        const auto nextIdx = static_cast<std::uint8_t>((m_primarySlotIndex.load(std::memory_order_relaxed) ^ 1u) & 1u);
        m_primarySlotIndex.store(nextIdx, std::memory_order_release);
        return m_slots[nextIdx];
    }

    [[nodiscard]] DecodePipeline &decodePipeline()
    {
        return primarySlot().decodePipeline;
    }
    [[nodiscard]] Pipe<float> &audioPipe()
    {
        return primarySlot().audioPipe;
    }

    // Role routing for the decode-pipeline callbacks: only the primary
    // slot's sessions may affect player and sink state.
    [[nodiscard]] bool isPrimarySlot(const PipelineSlot &slot) const
    {
        return &slot == &primarySlot();
    }

    void invalidateSlotContent(PipelineSlot &slot)
    {
        slot.contentEpoch.fetch_add(1, std::memory_order_acq_rel);
    }

    void writeToSlotQueue(PipelineSlot &slot, std::span<const float> pcm, const std::stop_token &st);

    bool onPipelineBoundary(const DecodePipeline::BoundaryEvent &event);

    struct PendingTrack {
        QUrl url;
        int sampleRate = 0;
        int channels = 0;
        std::optional<std::chrono::milliseconds> duration = std::nullopt;
        bool seekable = false;
    };
    QHash<std::uint32_t, PendingTrack> m_pendingTracks;
    mutable std::mutex m_pendingTracksMutex;
    std::atomic<std::uint32_t> m_trackSerial{0};

    // Applies a pending track switch to the player's reported state (source,
    // duration, seekability, format). Used both at play time (the sink
    // crossed a TrackStart boundary) and at seek time (the seek's flush
    // would drop the boundary, so the switch is resolved eagerly).
    void applyPendingTrack(const PendingTrack &pending);

    // Resolves the pending track switch, if any, from within setPosition():
    // the decoder is already on the promoted track while the player still
    // reports the old one, and the seek's flush drops the pending boundary.
    // Must run after requestSeek() (which waits out an in-flight promotion
    // and thereby guarantees the pending entry exists) and before the flush
    // is armed.
    void resolvePendingTrackForSeek();

    // The stream epoch the decode thread's producer last observed before a
    // write (see PlaybackPipeline::streamEpoch()). Decode-thread confined
    // writer; read on the decode thread when recording boundaries. A
    // TrackStart boundary is stamped with it so boundaries of a superseded
    // stream are discarded instead of firing in the new one.
    std::atomic<std::uint64_t> m_producerStreamEpoch{0};

    void invalidateQueuedContent();

    // Shared teardown sequence: abandon any staged/active crossfade, close
    // the sink (quiescing the audio callback) and fully reset mixer + clock
    // (which also un-sticks a stale EOS claim state).
    void teardownPipeline();

    QCoro::Task<void> startLoad(PipelineSlot &slot, QUrl source, uint64_t generation, Decoder::TargetAudioFormat targetFormat = {});

    void setPlaybackState(DragonPlayer::PlaybackState state);
    void setStatus(DragonPlayer::MediaStatus status);
    void setError(DragonPlayer::Error error, const QString &message = {});

    void stopPipeline();

    QUrl currentSource;
    DragonSourceOptions currentSourceOptions;
    QUrl nextSource;
    DragonPlayer::PlaybackState currentPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    DragonPlayer::MediaStatus currentStatus = DragonPlayer::MediaStatus::NoMedia;
    DragonPlayer::Error currentError = DragonPlayer::Error::NoError;
    QString currentErrorString;
    DragonPlayer::PlaybackState requestedPlaybackState = DragonPlayer::PlaybackState::StoppedState;
    std::optional<std::chrono::milliseconds> currentDuration{};
    bool currentSeekable = false;
    bool currentIsLocal = false;
    int currentSampleRate = 0;
    int currentChannels = 0;
    qreal currentBufferProgress = 1.0;
    DragonAudioOutput::Backend requestedBackend = DragonAudioOutput::Backend::Auto;

    std::shared_ptr<AliveGuard> aliveGuard;

    PositionEstimator *positionEstimator = nullptr;

    bool playRequestedReload = false;

    uint64_t loadGeneration = 0;

    std::chrono::milliseconds prefinishMark{2000ms};
    bool aboutToFinishEmitted = false;
};
