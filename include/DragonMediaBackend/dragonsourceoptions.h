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
#include <QtQmlIntegration/qqmlintegration.h>

#include <chrono>

class DragonSourceOptionsPrivate;

/*!
 * \class DragonSourceOptions
 * \inmodule DragonMediaBackend
 * \inheaderfile DragonSourceOptions
 *
 * \brief Per-source playback options, currently the crossfade duration.
 *
 * DragonSourceOptions is passed to the
 * \l [cpp] DragonPlayer::setSource(const QUrl &, const DragonSourceOptions &)
 * overload to request an overlapped crossfade when switching between
 * tracks while playing.
 *
 * The class is implicitly shared, so copying one is cheap. Setting a
 * duration detaches the shared data, following copy-on-write semantics.
 *
 * \sa DragonPlayer::setSource()
 */

/*!
 * \qmlvaluetype dragonSourceOptions
 * \nativetype DragonSourceOptions
 * \inqmlmodule org.kde.dragonmediabackend
 *
 * \brief Per-source playback options for \l DragonPlayer::setSource.
 *
 * dragonSourceOptions carries the crossfade duration used when
 * DragonPlayer::setSource switches between two tracks:
 *
 * \qml
 * player.setSource(nextUrl, dragonSourceOptions {
 *     crossfadeDuration.milliseconds: 1500
 * })
 * \endqml
 */

class DRAGONMEDIABACKEND_EXPORT DragonSourceOptions
{
    Q_GADGET
    QML_VALUE_TYPE(dragonSourceOptions)

    /*!
     * \qmlproperty dragonDuration org.kde.dragonmediabackend::dragonSourceOptions::crossfadeDuration
     *
     * How long the crossfade between tracks takes.
     */

    /*!
     * \property DragonSourceOptions::crossfadeDuration
     *
     * How long the crossfade between tracks takes.
     */
    Q_PROPERTY(DragonDuration crossfadeDuration READ crossfadeDuration WRITE setCrossfadeDuration)

public:
    /*!
     * Constructs options without any crossfade.
     */
    DragonSourceOptions();

    /*!
     * Copies \a other. The underlying data is implicitly shared.
     */
    DragonSourceOptions(const DragonSourceOptions &other);

    /*!
     * Move-constructs from \a other.
     */
    DragonSourceOptions(DragonSourceOptions &&other) noexcept;

    /*!
     * Destroys the options.
     */
    ~DragonSourceOptions();

    /*!
     * Assigns \a other to this object. The underlying data is implicitly
     * shared.
     */
    DragonSourceOptions &operator=(const DragonSourceOptions &other);

    /*!
     * Move-assigns \a other to this object.
     */
    DragonSourceOptions &operator=(DragonSourceOptions &&other) noexcept;

    /*!
     * Returns \c true if \a other requests the same crossfade duration.
     */
    [[nodiscard]] bool operator==(const DragonSourceOptions &other) const;

    /*!
     * Returns \c true if \a other requests a different crossfade duration.
     */
    [[nodiscard]] bool operator!=(const DragonSourceOptions &other) const;

    /*!
     * Returns the crossfade duration.
     */
    // NOLINTNEXTLINE(clazy-qproperty-type-mismatch) DragonDuration is the QML adaptation of std::chrono::milliseconds
    [[nodiscard]] std::chrono::milliseconds crossfadeDuration() const;

    /*!
     * Sets the crossfade duration to \a duration. Negative durations
     * are clamped to zero.
     */
    // NOLINTNEXTLINE(clazy-qproperty-type-mismatch)
    void setCrossfadeDuration(std::chrono::milliseconds duration);

private:
    QSharedDataPointer<DragonSourceOptionsPrivate> d;
};

Q_DECLARE_METATYPE(DragonSourceOptions)
