/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonfftframe.h"
#include "dragonsdl_export.h"

#include <dragonsdl/dragonicymetadata.h>

#include <QCoroTask>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

class DragonPlayerPrivate;

class DRAGONSDL_EXPORT DragonPlayer : public QObject
{
    Q_OBJECT

public:
    enum class PlaybackState {
        StoppedState,
        PlayingState,
        PausedState
    };
    Q_ENUM(PlaybackState)

    enum class MediaStatus {
        NoMedia,
        LoadingMedia,
        LoadedMedia,
        BufferingMedia,
        StalledMedia,
        BufferedMedia,
        EndOfMedia,
        InvalidMedia
    };
    Q_ENUM(MediaStatus)

    enum class Error {
        NoError,
        ResourceError,
        FormatError,
        NetworkError,
        AccessDenied
    };
    Q_ENUM(Error)

    enum class FftMode {
        Off,
        BarsOnly,
        DetailedOnly,
        Both
    };
    Q_ENUM(FftMode)

    explicit DragonPlayer(QObject *parent = nullptr);
    ~DragonPlayer() override;

    DragonPlayer(const DragonPlayer &) = delete;
    DragonPlayer &operator=(const DragonPlayer &) = delete;
    DragonPlayer(DragonPlayer &&) = delete;
    DragonPlayer &operator=(DragonPlayer &&) = delete;

    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QUrl nextSource READ nextSource WRITE setNextSource NOTIFY nextSourceChanged)
    Q_PROPERTY(PlaybackState playbackState READ playbackState NOTIFY playbackStateChanged)
    Q_PROPERTY(MediaStatus status READ status NOTIFY statusChanged)
    Q_PROPERTY(Error error READ error NOTIFY errorChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(int64_t position READ position WRITE setPosition NOTIFY positionChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(FftMode fftMode READ fftMode WRITE setFftMode NOTIFY fftModeChanged)

    [[nodiscard]] bool muted() const;
    [[nodiscard]] float volume() const;
    [[nodiscard]] QUrl source() const;
    [[nodiscard]] QUrl nextSource() const;
    [[nodiscard]] PlaybackState playbackState() const;
    [[nodiscard]] MediaStatus status() const;
    [[nodiscard]] Error error() const;
    [[nodiscard]] int64_t duration() const;
    [[nodiscard]] int64_t position() const;
    [[nodiscard]] bool seekable() const;

    [[nodiscard]] bool isAudioActive() const;

    [[nodiscard]] FftMode fftMode() const;

Q_SIGNALS:
    void mutedChanged(bool muted);
    void volumeChanged();
    void sourceChanged();
    void nextSourceChanged();
    void trackChanged();
    void playbackStateChanged(DragonPlayer::PlaybackState state);
    void statusChanged(DragonPlayer::MediaStatus status);
    void errorChanged(DragonPlayer::Error error);
    void durationChanged(int64_t durationMs);
    void positionChanged(int64_t positionMs);
    void seekableChanged(bool seekable);
    void fftModeChanged(DragonPlayer::FftMode mode);

    void playingChanged(bool playing);

    void playing();
    void paused();
    void stopped();

    void fftFrameReady(const DragonFftFrame &frame);

    void currentPlayingForRadiosChanged(const DragonIcyMetadata &metadata);

public Q_SLOTS:
    void setMuted(bool muted);
    void setVolume(float linearGain);

    QCoro::Task<void> setSource(QUrl source);
    void setNextSource(const QUrl &nextSource);
    void setPosition(int64_t positionMs);
    void setFftMode(FftMode mode);

    void play();
    void pause();
    void stop();
    void seek(int64_t positionMs);

    void saveUndoPosition(int64_t positionMs);
    void restoreUndoPosition();

    friend class DragonDiagnostics;
    friend class DragonPlayerPrivate;

private:
    std::unique_ptr<DragonPlayerPrivate> d;
};