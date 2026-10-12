/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragoncdtoc.h"
#include "dragonmediabackend_export.h"

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <memory>

class DragonCdTocReaderPrivate;

/*!
 * \class DragonCdTocReader
 * \inmodule DragonMediaBackend
 * \inheaderfile DragonCdTocReader
 *
 * \brief Reads the table of contents of an audio CD asynchronously.
 *
 * Call requestToc(), then handle tocReady() and the failed() signals.
 *
 * With the TOC in hand, the application can build its track list and
 * run a CDDB or MusicBrainz lookup itself — the reader supplies the
 * basic track data, it does not do network metadata lookups.
 *
 * Re-reading after a disc swap requires another requestToc() call. Use
 * \l {https://api.kde.org/solid-index.html}{Solid} to detect when discs are
 * inserted or removed.
 *
 * Playback of the returned tracks is \l DragonPlayer's job: set
 * \l {DragonPlayer::source}{source} to a track's
 * \l {DragonCdTrack::url()}{url()}.
 *
 * \sa DragonCdToc, DragonCdTrack, DragonPlayer
 */

/*!
 * \qmltype DragonCdTocReader
 * \instantiates DragonCdTocReader
 * \inqmlmodule org.kde.dragonmediabackend
 *
 * \brief Reads the table of contents of an audio CD asynchronously.
 *
 * \qml
 * import org.kde.dragonmediabackend
 *
 * DragonCdTocReader {
 *     id: cdReader
 *
 *     onTocReady: (toc) => {
 *         discLabel.text = i18n("%1 tracks, %2",
 *                               toc.tracks.length,
 *                               toc.discDuration.formatted())
 *         player.source = toc.tracks[0].url
 *     }
 *     onFailed: (message) => errorLabel.text = message
 * }
 *
 * DragonPlayer {
 *     id: player
 * }
 * \endqml
 *
 * \sa DragonCdToc, DragonCdTocReader::tocReady, DragonCdTocReader::failed
 */

class DRAGONMEDIABACKEND_EXPORT DragonCdTocReader : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(DragonCdTocReader)

public:
    /*!
     * Constructs a TOC reader with the given \a parent.
     */
    explicit DragonCdTocReader(QObject *parent = nullptr);

    /*!
     * Destroys the reader. A TOC read still in flight is abandoned; no
     * signal is emitted for it.
     */
    ~DragonCdTocReader() override;

    DragonCdTocReader(const DragonCdTocReader &) = delete;
    DragonCdTocReader &operator=(const DragonCdTocReader &) = delete;

    /*!
     * \qmlmethod void org.kde.dragonmediabackend::DragonCdTocReader::requestToc(string device)
     *
     * Reads the TOC of \a device asynchronously and emits tocReady() or
     * failed(). An empty \a device selects the default drive.
     */

    /*!
     * Reads the TOC of \a device asynchronously and emits tocReady() or
     * failed(). An empty \a device selects the default CD drive.
     *
     * Re-reading after a disc swap is a plain second requestToc() call;
     * the backend's disc cache handles media-change detection. Request
     * the TOC of the same device while a read is in flight coalesces
     * into that read, so it produces a single tocReady().
     */
    Q_INVOKABLE void requestToc(const QString &device = QString());

Q_SIGNALS:
    /*!
     * \qmlsignal void org.kde.dragonmediabackend::DragonCdTocReader::tocReady(dragonCdToc toc)
     *
     * Emitted with the TOC \a toc of the requested device once it has
     * been read. Spinning up an optical drives might take a couple of seconds.
     *
     * Handle it with \c {onTocReady: (toc) => ...}.
     */

    /*!
     * Emitted with the TOC \a toc of the requested device once it has
     * been read. Spinning up an optical drives might take a couple of seconds.
     */
    void tocReady(const DragonCdToc &toc);

    /*!
     * \qmlsignal void org.kde.dragonmediabackend::DragonCdTocReader::failed(string errorMessage)
     *
     * Emitted with the reason \a errorMessage when the TOC could not be read:
     * no disc, not an Audio CD, an unopenable device, or the library was built without CD support.
     */

    /*!
     * Emitted with the reason \a errorMessage when the TOC could not be read:
     * no disc, not an Audio CD, an unopenable device, or the library was built without CD support.
     */
    void failed(const QString &errorMessage);

private:
    std::unique_ptr<DragonCdTocReaderPrivate> d;

    friend class DragonCdTocReaderPrivate;
    friend class DragonCdTocAcquireJob;
};
