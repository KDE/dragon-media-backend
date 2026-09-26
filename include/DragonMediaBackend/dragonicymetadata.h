/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QHash>
#include <QMetaType>
#include <QSharedDataPointer>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

class DragonIcyMetadataPrivate;

/*!
 * \class DragonIcyMetadata
 * \inmodule DragonMediaBackend
 *
 * \brief Implicit-shared \l {https://cast.readme.io/docs/icy} {ICY metadata} from radio streams.
 *
 * While an internet radio stream is played, the stream interleaves ICY
 * metadata blocks with the audio. Dragon Media Backend extracts them and
 * reports the currently playing track through
 * \l DragonPlayer::currentPlayingForRadiosChanged().
 *
 * The well-known fields streamTitle and streamUrl are exposed directly,
 * all other fields are available through customFields(). The class is
 * implicitly shared, so copying one is cheap.
 *
 * \sa DragonPlayer::currentPlayingForRadiosChanged()
 */
class DRAGONMEDIABACKEND_EXPORT DragonIcyMetadata
{
    Q_GADGET
    QML_VALUE_TYPE(dragonIcyMetadata)
    Q_PROPERTY(QString streamTitle READ streamTitle WRITE setStreamTitle)
    Q_PROPERTY(QString streamUrl READ streamUrl WRITE setStreamUrl)
    Q_PROPERTY(bool isNull READ isNull CONSTANT)

public:
    /*!
     * Constructs a null metadata object. isNull() returns \c true.
     */
    DragonIcyMetadata();

    /*!
     * Copies \a other. The underlying data is implicitly shared.
     */
    DragonIcyMetadata(const DragonIcyMetadata &other);

    /*!
     * Assigns \a other to this object. The underlying data is implicitly
     * shared.
     */
    DragonIcyMetadata &operator=(const DragonIcyMetadata &other);

    ~DragonIcyMetadata();

    /*!
     * Returns the ICY stream title, typically the artist and track name
     * of the song currently playing.
     *
     * Returns an empty string if the stream did not provide a title;
     * use hasStreamTitle() to distinguish that case.
     */
    [[nodiscard]] QString streamTitle() const;

    /*!
     * Returns \c true if the stream provided a stream title.
     */
    [[nodiscard]] bool hasStreamTitle() const;

    /*!
     * Sets the ICY stream title to \a title.
     */
    void setStreamTitle(const QString &title);

    /*!
     * Returns the ICY stream URL, usually the stream's home page.
     *
     * Returns an empty string if the stream did not provide a URL;
     * use hasStreamUrl() to distinguish that case.
     */
    [[nodiscard]] QString streamUrl() const;

    /*!
     * Returns \c true if the stream provided a stream URL.
     */
    [[nodiscard]] bool hasStreamUrl() const;

    /*!
     * Sets the ICY stream URL to \a url.
     */
    void setStreamUrl(const QString &url);

    /*!
     * Returns all custom ICY fields the stream provided, as a mapping
     * of field name to value.
     */
    [[nodiscard]] QHash<QString, QString> customFields() const;

    /*!
     * Replaces the custom ICY fields with \a fields.
     */
    void setCustomFields(const QHash<QString, QString> &fields);

    /*!
     * Inserts the custom ICY field \a key with \a value, replacing any
     * previous value stored under \a key.
     */
    void insertCustomField(const QString &key, const QString &value);

    /*!
     * Returns \c true if this object was default-constructed and no
     * metadata was set.
     */
    [[nodiscard]] bool isNull() const;

    /*!
     * Clears all metadata, making this object null.
     */
    void clear();

    /*!
     * Returns \c true if \a other holds the same metadata.
     */
    [[nodiscard]] bool operator==(const DragonIcyMetadata &other) const;

    /*!
     * Returns \c true if \a other holds different metadata.
     */
    [[nodiscard]] bool operator!=(const DragonIcyMetadata &other) const;

private:
    QSharedDataPointer<DragonIcyMetadataPrivate> d;
};

Q_DECLARE_METATYPE(DragonIcyMetadata)
