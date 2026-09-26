/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonduration.h"
#include "dragonfftframe.h"
#include "dragonmediabackend_export.h"

#include <DragonMediaBackend/dragonaudiooutput.h>
#include <DragonMediaBackend/dragonicymetadata.h>

#include <QObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <memory>
#include <optional>

class DragonAudioOutput;
class DragonPlayerPrivate;

/*!
 * \class DragonPlayer
 * \inmodule DragonMediaBackend
 *
 * \brief Audio playback controller.
 *
 * DragonPlayer is the main entry point of Dragon Media Backend. It manages
 * the state machine for media selection and playback of audio files.
 *
 * It can support gapless playback between sequential tracks: when aboutToFinish() is
 * emitted the application can set nextSource() and it will play that track immediately
 * after the current track is finished..
 *
 * \sa DragonAudioOutput, DragonSpectrumAnalyzer, DragonIcyMetadata
 */
class DRAGONMEDIABACKEND_EXPORT DragonPlayer : public QObject
{
    Q_OBJECT

public:
    /*!
     * \enum DragonPlayer::PlaybackState
     *
     * The playback state of the player.
     *
     * \value StoppedState
     *        No media is being played.
     * \value PlayingState
     *        Media is being played.
     * \value PausedState
     *        Playback is paused and can be resumed with play().
     */
    enum class PlaybackState {
        StoppedState,
        PlayingState,
        PausedState
    };
    Q_ENUM(PlaybackState)

    /*!
     * \enum DragonPlayer::MediaStatus
     *
     * The status of the loaded media.
     *
     * \value NoMedia
     *        No source has been set.
     * \value LoadingMedia
     *        The source is being opened and probed.
     * \value LoadedMedia
     *        The source is loaded and possibly playing.
     * \value BufferingMedia
     *        Buffering the network stream.
     * \value StalledMedia
     *        The network stream has stalled.
     * \value BufferedMedia
     *        Enough of a network stream is buffered for playback.
     * \value EndOfMedia
     *        Playback reached the end of the source.
     * \value InvalidMedia
     *        Something if off; check the DragonPlayer::error.
     */
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

    /*!
     * \enum DragonPlayer::Error
     *
     * The error state of the player.
     *
     * \value NoError
     *        No error occurred.
     * \value ResourceError
     *        An audio resource could not be acquired.
     * \value FormatError
     *        The media format is not supported.
     * \value NetworkError
     *        A network error occurred while streaming.
     * \value AccessDenied
     *        Access to the source is denied.
     */
    enum class Error {
        NoError,
        ResourceError,
        FormatError,
        NetworkError,
        AccessDenied
    };
    Q_ENUM(Error)

    /*!
     *
     * Constructs the main controller. The audio output backends  - PipeWire, PulseAudio, SDL3
     * - are probed in that order until a functional backend is found. Note that SDL Audio
     * itself has a similar driver abstraction layer with multiple backends and is likely
     * used anywhere outside of the Linux/Unix desktop environment.
     */
    explicit DragonPlayer(QObject *parent = nullptr);

    /*!
     * Constructs a player using the audio backend \a requestedBackend,
     * falling back to probing when it is not available.
     */
    explicit DragonPlayer(DragonAudioOutput::Backend requestedBackend, QObject *parent = nullptr);
    ~DragonPlayer() override;

    DragonPlayer(const DragonPlayer &) = delete;
    DragonPlayer &operator=(const DragonPlayer &) = delete;
    DragonPlayer(DragonPlayer &&) = delete;
    DragonPlayer &operator=(DragonPlayer &&) = delete;

    /*!
     * \property DragonPlayer::source
     *
     * The media being played. If it's currently playing, setting a new source
     * stops playback, resets the position, and starts loading the new media.
     */
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)

    /*!
     * \property DragonPlayer::nextSource
     *
     * The source played when the current track finishes, used for
     * gapless transitions. Set it in response to aboutToFinish().
     *
     * \sa aboutToFinish()
     */
    Q_PROPERTY(QUrl nextSource READ nextSource WRITE setNextSource NOTIFY nextSourceChanged)

    /*!
     * \property DragonPlayer::playbackState
     *
     * The current playback state.
     */
    Q_PROPERTY(PlaybackState playbackState READ playbackState NOTIFY stateChanged)

    /*!
     * \property DragonPlayer::status
     *
     * The status of the loaded media.
     */
    Q_PROPERTY(MediaStatus status READ status NOTIFY statusChanged)

    /*!
     * \property DragonPlayer::error
     *
     * The error state of the player, NoError when there is no error.
     */
    Q_PROPERTY(Error error READ error NOTIFY errorChanged)

    /*!
     * \property DragonPlayer::errorString
     *
     * A human-readable description of the current error, if any.
     */
    Q_PROPERTY(QString errorString READ errorString NOTIFY errorChanged)

    /*!
     * \property DragonPlayer::duration
     *
     * The duration of the current source as a \l DragonDuration, which
     * is \l {DragonDuration::valid}{invalid} for streams of unknown
     * duration. C++ code should use duration(), which reports unknown
     * durations as an empty std::optional.
     */
    Q_PROPERTY(DragonDuration duration READ duration NOTIFY durationChanged)

    /*!
     * \property DragonPlayer::position
     *
     * The playback position as a \l DragonDuration. Setting it while a
     * source is loaded seeks playback. C++ code should use position()
     * and setPosition() with std::chrono::milliseconds.
     */
    Q_PROPERTY(DragonDuration position READ position WRITE setPosition NOTIFY positionChanged)

    /*!
     * \property DragonPlayer::seekable
     *
     * Whether the playback position of the current source can be
     * changed. Radio streams are typically not seekable.
     */
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)

    /*!
     * \property DragonPlayer::bufferProgress
     *
     * How much of a network stream has been buffered, from 0.0 to 1.0.
     * Always 1.0 for fully buffered local files.
     */
    Q_PROPERTY(qreal bufferProgress READ bufferProgress NOTIFY bufferProgressChanged)

    /*!
     * \property DragonPlayer::prefinishMark
     *
     * How long before the end of the source aboutToFinish() is emitted,
     * allowing the next source to be queued for a gapless transition.
     * The default is 0 milliseconds, meaning aboutToFinish() is only
     * emitted when decoding finishes with no time remaining. C++ code
     * should use prefinishMark() and setPrefinishMark() with
     * std::chrono::milliseconds.
     *
     * \sa aboutToFinish()
     */
    Q_PROPERTY(DragonDuration prefinishMark READ prefinishMark WRITE setPrefinishMark NOTIFY prefinishMarkChanged)

    /*!
     * Returns the \l DragonAudioOutput instance controlling volume and
     * mute for this player. The instance is owned by the player.
     */
    [[nodiscard]] DragonAudioOutput *audioOutput() const;

    /*!
     * Returns the media being played.
     */
    [[nodiscard]] QUrl source() const;

    /*!
     * Returns the source queued for the gapless transition at the end
     * of the current track, or an empty URL if none is set.
     */
    [[nodiscard]] QUrl nextSource() const;

    /*!
     * Returns the current playback state.
     */
    [[nodiscard]] PlaybackState playbackState() const;

    /*!
     * Returns the status of the loaded media.
     */
    [[nodiscard]] MediaStatus status() const;

    /*!
     * Returns the error state of the player.
     */
    [[nodiscard]] Error error() const;

    /*!
     * Returns a human-readable description of the current error, if any.
     */
    [[nodiscard]] QString errorString() const;

    /*!
     * Returns the duration of the current source, or an empty
     * std::optional for streams of unknown duration.
     */
    [[nodiscard]] std::optional<std::chrono::milliseconds> duration() const;

    /*!
     * Returns the playback position.
     */
    [[nodiscard]] std::chrono::milliseconds position() const;

    /*!
     * Returns whether the current source is seekable.
     */
    [[nodiscard]] bool seekable() const;

    /*!
     * Returns how much of a network stream has been buffered, from 0.0
     * to 1.0.
     */
    [[nodiscard]] qreal bufferProgress() const;

    /*!
     * Returns how long before the end of the source aboutToFinish() is
     * emitted.
     */
    [[nodiscard]] std::chrono::milliseconds prefinishMark() const;

Q_SIGNALS:
    /*!
     * Emitted when the source property changes.
     */
    void sourceChanged();

    /*!
     * Emitted when the queued next source changes.
     */
    void nextSourceChanged();

    /*!
     * Emitted when the playing track changes, for example on a gapless
     * transition or a source change.
     */
    void trackChanged();

    /*!
     * Emitted when the playback state changes to \a newState, with the
     * previous state in \a oldState.
     */
    void stateChanged(PlaybackState newState, PlaybackState oldState);

    /*!
     * Emitted when the media status changes to \a status.
     */
    void statusChanged(MediaStatus status);

    /*!
     * Emitted when the error state changes to \a error.
     */
    void errorChanged(Error error);

    /*!
     * Emitted when the duration of the current source changes to \a
     * duration. The optional is empty for streams of unknown duration.
     */
    void durationChanged(std::optional<std::chrono::milliseconds> duration);

    /*!
     * Emitted while playing, when the playback position changes to \a
     * position.
     */
    void positionChanged(std::chrono::milliseconds position);

    /*!
     * Emitted when the seekable state of the current source changes to
     * \a seekable.
     */
    void seekableChanged(bool seekable);

    /*!
     * Emitted when the buffered amount of a network stream changes,
     * with the new fraction in \a progress.
     */
    void bufferProgressChanged(qreal progress);

    /*!
     * Emitted when the prefinish mark changes to \a msec.
     */
    void prefinishMarkChanged(std::chrono::milliseconds msec);

    /*!
     * Emitted prefinishMark() milliseconds before the current source
     * finishes. Set nextSource() in response to queue the next track
     * for a gapless transition.
     *
     * \sa nextSource, setNextSource(), prefinishMark
     */
    void aboutToFinish();

    /*!
     * Emitted when the ICY metadata of a radio stream changes, with the
     * new \a metadata. Typically reports the track currently playing.
     */
    void currentPlayingForRadiosChanged(const DragonIcyMetadata &metadata);

public Q_SLOTS:
    /*!
     * Sets the media to be played to \a source. Any current playback is
     * stopped and the position is reset.
     */
    void setSource(const QUrl &source);

    /*!
     * Queues \a nextSource to be played when the current source
     * finishes, providing a gapless transition.
     */
    void setNextSource(const QUrl &nextSource);

    /*!
     * Sets the display name of the stream being played to \a name. Used
     * when reporting ICY metadata for radio streams.
     */
    void setStreamName(const QString &name);

    /*!
     * Seeks to \a position into the current source, when it is seekable.
     */
    void setPosition(std::chrono::milliseconds position);

    /*!
     * Sets the prefinish mark to \a msec.
     *
     * \sa aboutToFinish()
     */
    void setPrefinishMark(std::chrono::milliseconds msec);

    /*!
     * Starts or resumes playback of the current source.
     */
    void play();

    /*!
     * Pauses playback. Call play() to resume from the same position.
     */
    void pause();

    /*!
     * Stops playback and resets the position. The source remains loaded
     * and playback can be restarted with play().
     */
    void stop();

private:
    friend class DragonDiagnostics;
    friend class DragonPlayerPrivate;
    friend class DragonSpectrumAnalyzer;
    friend class DragonSpectrumAnalyzerPrivate;

private:
    std::unique_ptr<DragonPlayerPrivate> d;
};
