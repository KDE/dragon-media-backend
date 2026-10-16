/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#ifdef DRAGONMEDIABACKEND_INTERNAL_BUILD
#include "player/dragonduration.h"
#else
class DragonDuration;
#endif

#include <QMetaType>
#include <QSharedDataPointer>
#include <QString>
#include <QUrl>

#include <chrono>

class DragonCdTrackPrivate;
class DragonCdTocPrivate;

/*!
 * \class DragonCdTrack
 * \inmodule DragonMediaBackend
 * \inheaderfile DragonCdTrack
 *
 * \brief One track of an audio CD
 *
 * Includes the basic information available from a audio CD's table of contents.
 *
 * \sa DragonCdToc, DragonCdTocReader
 */

/*!
 * \qmlvaluetype dragonCdTrack
 * \nativetype DragonCdTrack
 * \inqmlmodule org.kde.dragonmediabackend
 *
 * \brief One track of an audio CD
 *
 * Includes the basic information available from a audio CD's table of contents.
 */

class DRAGONMEDIABACKEND_EXPORT DragonCdTrack
{
    Q_GADGET

    /*!
     * \qmlproperty int org.kde.dragonmediabackend::dragonCdTrack::number
     *
     * The track number as reported by the disc (1..99), not a list index.
     */

    /*!
     * \property DragonCdTrack::number
     *
     * The track number as reported by the disc (1..99), not a list index.
     */
    Q_PROPERTY(int number READ number CONSTANT FINAL)

    /*!
     * \qmlproperty bool org.kde.dragonmediabackend::dragonCdTrack::audio
     *
     * Whether this track holds audio. \c false for the data track of a
     * mixed-mode disc.
     */

    /*!
     * \property DragonCdTrack::audio
     *
     * Whether this track holds audio, \c false for the data track of a
     * mixed-mode disc.
     */
    Q_PROPERTY(bool audio READ isAudio CONSTANT FINAL)

    /*!
     * \qmlproperty int org.kde.dragonmediabackend::dragonCdTrack::startFrame
     *
     * The start position of the track in audio frames (sectors): 75 frames
     * equal one second.
     */

    /*!
     * \property DragonCdTrack::startFrame
     *
     * The start position of the track in audio frames (sectors): 75 frames
     * equal one second.
     */
    Q_PROPERTY(int startFrame READ startFrame CONSTANT FINAL)

    /*!
     * \qmlproperty int org.kde.dragonmediabackend::dragonCdTrack::lengthFrames
     *
     * The length of the track in frames (75 frames equal one second).
     */

    /*!
     * \property DragonCdTrack::lengthFrames
     *
     * The length of the track in frames; \c lengthFrames() * 1000 / 75 is
     * the duration in milliseconds.
     */
    Q_PROPERTY(int lengthFrames READ lengthFrames CONSTANT FINAL)

    /*!
     * \qmlproperty dragonDuration org.kde.dragonmediabackend::dragonCdTrack::duration
     *
     * The duration of the track.
     */

    /*!
     * \property DragonCdTrack::duration
     *
     * The duration of the track.
     */
    Q_PROPERTY(DragonDuration duration READ duration CONSTANT FINAL)

    /*!
     * \qmlproperty url org.kde.dragonmediabackend::dragonCdTrack::url
     *
     * The URL that plays this track through DragonPlayer, for example
     * \c {audiocd:/Track 03.wav?track=3&device=/dev/sr0}. Empty for data tracks.
     */

    /*!
     * \property DragonCdTrack::url
     *
     * The URL that plays this track through DragonPlayer, for example
     * \c {audiocd:/Track 03.wav?track=3&device=/dev/sr0}. Empty for data tracks.
     */
    Q_PROPERTY(QUrl url READ url CONSTANT FINAL)

public:
    /*!
     * Constructs a null track with number 0, an empty url, and a zero
     * duration.
     */
    DragonCdTrack();

    /*! Copies \a other. The underlying data is implicitly shared. */
    DragonCdTrack(const DragonCdTrack &other);

    /*! Move-constructs the track of \a other, which is left as a placeholder. */
    DragonCdTrack(DragonCdTrack &&other) noexcept;

    /*! Assigns \a other to this object. The underlying data is implicitly shared. */
    DragonCdTrack &operator=(const DragonCdTrack &other);

    /*! Move-assigns the track of \a other, which is left as a placeholder. */
    DragonCdTrack &operator=(DragonCdTrack &&other) noexcept;

    ~DragonCdTrack();

    /*!
     * Returns \c true if \a other describes the same track of the same
     * disc.
     */
    [[nodiscard]] bool operator==(const DragonCdTrack &other) const;

    /*!
     * Returns the track number as reported by the TOC (1..99; not a list
     * index).
     */
    [[nodiscard]] int number() const;

    /*!
     * Returns \c false for the data track of a mixed-mode disc.
     */
    [[nodiscard]] bool isAudio() const;

    /*!
     * Returns the start position in frames. Useful for CDDB lookups.
     */
    [[nodiscard]] int startFrame() const;

    /*!
     * Returns the length of the track in frames.
     */
    [[nodiscard]] int lengthFrames() const;

    /*!
     * Returns the duration of the track. The
     * value serves the \l duration property through the implicit
     * conversion of DragonDuration.
     */
    [[nodiscard]] std::chrono::milliseconds duration() const;

    /*!
     * Returns the playable URL of this track:
     * \c {audiocd:/Track NN.wav?track=N&device=...} matching the scheme
     * DragonCdStream parses. Data tracks report an empty URL.
     */
    [[nodiscard]] QUrl url() const;

private:
    explicit DragonCdTrack(DragonCdTrackPrivate *trackPrivate);

    QSharedDataPointer<DragonCdTrackPrivate> d;

    friend class DragonCdTocPrivate;
    friend class DragonCdDisc;
};

Q_DECLARE_METATYPE(DragonCdTrack)
