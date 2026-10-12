/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragoncdtrack.h"
#include "dragonmediabackend_export.h"

#ifdef DRAGONMEDIABACKEND_INTERNAL_BUILD
#include "player/dragonduration.h"
#else
class DragonDuration;
#endif

#include <QList>
#include <QMetaType>
#include <QSharedDataPointer>
#include <QString>

#include <chrono>

class DragonCdTocPrivate;
class DragonCdTocReaderPrivate;

/*!
 *
 * \class DragonCdToc
 * \inmodule DragonMediaBackend
 * \inheaderfile DragonCdToc
 *
 * \brief The table of contents of an audio CD.
 *
 * DragonCdToc contains the 'table of contents' of the audio CD.
 * The table of contents is essential metadata necessary to play the CD. It's located at
 * the very start of the CD. Learn more at the {https://en.wikipedia.org/wiki/Compact_Disc_Digital_Audio#Technical_specifications}
 * {Wikipedia article on Audio CD spec}.
 *
 * Using this metadata you can look up album, artist and track titles from the Internet, with a library like KCDDB.
 *
 * Obtain a DragonCdToc from the \l [cpp] DragonCdTocReader::tocReady() signal.
 *
 * \sa DragonCdTrack, DragonCdTocReader
 */

/*!
 * \qmlvaluetype dragonCdToc
 * \nativetype DragonCdToc
 * \inqmlmodule org.kde.dragonmediabackend
 *
 * \brief The table of contents of an audio CD.
 *
 * dragonCdToc values are delivered by the \l {org.kde.dragonmediabackend::DragonCdTocReader::tocReady}{DragonCdTocReader.tocReady} signal:
 *
 * \qml
 * DragonCdTocReader {
 *     id: cdReader
 *     onTocReady: (toc) => {
 *         discLabel.text = i18n("%1 tracks, %2",
 *                               toc.tracks.length,
 *                               toc.discDuration.formatted())
 *         player.source = toc.tracks[0].url
 *     }
 *     onFailed: (message) => errorLabel.text = message
 * }
 * \endqml
 *
 */

class DRAGONMEDIABACKEND_EXPORT DragonCdToc
{
    Q_GADGET

    /*!
     * \qmlproperty string org.kde.dragonmediabackend::dragonCdToc::device
     *
     * The device this TOC was read from.
     */

    /*!
     * \property DragonCdToc::device
     *
     * The device this TOC was read from.
     */
    Q_PROPERTY(QString device READ device CONSTANT)

    /*!
     * \qmlproperty int org.kde.dragonmediabackend::dragonCdToc::leadOutFrame
     *
     * The lead-out position of the disc in audio frames (also called sectors), the end of the program portion of the disc.
     *
     * In CDDB terms, it is the disc length.
     *
     * Learn more at the \l {https://en.wikipedia.org/wiki/Track_(optical_disc)}{Wikipedia article on optical disc tracks}.
     */

    /*!
     * \property DragonCdToc::leadOutFrame
     *
     * The lead-out position of the disc in audio frames (also called sectors), the end of the program portion of the disc.
     *
     * In CDDB terms, it is the disc length.
     *
     * Learn more at the \l {https://en.wikipedia.org/wiki/Track_(optical_disc)}{Wikipedia article on optical disc tracks}.
     */
    Q_PROPERTY(int leadOutFrame READ leadOutFrame CONSTANT)

    /*!
     * \qmlproperty dragonDuration org.kde.dragonmediabackend::dragonCdToc::discDuration
     *
     * The total playable duration of the disc.
     */

    /*!
     * \property DragonCdToc::discDuration
     *
     * The total playable duration of the disc.
     */
    Q_PROPERTY(DragonDuration discDuration READ discDuration CONSTANT)

    /*!
     * \qmlproperty list<dragonCdTrack> org.kde.dragonmediabackend::dragonCdToc::tracks
     *
     * All tracks of the disc in TOC order.
     */

    /*!
     * \property DragonCdToc::tracks
     *
     * All tracks of the disc in TOC order.
     */
    Q_PROPERTY(QList<DragonCdTrack> tracks READ tracks CONSTANT)

public:
    /*!
     * Constructs a placeholder TOC with no tracks and a zero duration.
     * Real TOCs arrive via \l [cpp] DragonCdTocReader::tocReady().
     */
    DragonCdToc();

    /*! Copies \a other. The underlying data is implicitly shared. */
    DragonCdToc(const DragonCdToc &other);

    /*! Move-constructs the TOC of \a other, which is left as a placeholder. */
    DragonCdToc(DragonCdToc &&other) noexcept;

    /*! Assigns \a other to this object. The underlying data is implicitly shared. */
    DragonCdToc &operator=(const DragonCdToc &other);

    /*! Move-assigns the TOC of \a other, which is left as a placeholder. */
    DragonCdToc &operator=(DragonCdToc &&other) noexcept;

    ~DragonCdToc();

    /*!
     * Returns \c true if \a other describes the same disc.
     */
    [[nodiscard]] bool operator==(const DragonCdToc &other) const;

    /*!
     * The device this TOC was read from.
     */
    [[nodiscard]] QString device() const;

    /*!
     * Returns all tracks in TOC order.
     */
    [[nodiscard]] QList<DragonCdTrack> tracks() const;

    /*!
     * The lead-out position of the disc in audio frames (also called sectors), the end of the program portion of the disc.
     *
     * In CDDB terms, it is the disc length.
     *
     * Learn more at the \l {https://en.wikipedia.org/wiki/Track_(optical_disc)}{Wikipedia article on optical disc tracks}.
     *
     */
    [[nodiscard]] int leadOutFrame() const;

    /*!
     * Returns the total playable duration.
     */
    [[nodiscard]] std::chrono::milliseconds discDuration() const;

    /*!
     * Returns the CDDB signature of this disc: every track's start frame
     * in TOC order, followed by the lead-out frame. The list matches
     * \c {KCDDB::TrackOffsetList} and can be passed to
     * \c {KCDDB::Client::lookup()} unchanged.
     */
    [[nodiscard]] QList<quint32> cddbTrackOffsetList() const;

private:
    explicit DragonCdToc(DragonCdTocPrivate *tocPrivate);

    QSharedDataPointer<DragonCdTocPrivate> d;

    friend class DragonCdTocPrivate;
    friend class DragonCdDisc;
};

Q_DECLARE_METATYPE(DragonCdToc)
