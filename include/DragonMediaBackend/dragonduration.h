/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QObject>
#include <QString>
#include <QtQmlIntegration/qqmlintegration.h>

#include <chrono>
#include <optional>

/*!
 * \class DragonDuration
 * \inmodule DragonMediaBackend
 *
 * \brief Millisecond-precise duration value for QML.
 *
 * DragonDuration is the property-facing serialization of
 * std::chrono::milliseconds. It converts implicitly to and from
 * std::chrono::milliseconds (and from std::optional, where an empty
 * optional maps to an invalid duration), so the C++ API can keep
 * using std::chrono types while Q_PROPERTYs declared with
 * DragonDuration appear to QML as value objects with \l milliseconds,
 * \l seconds and \l valid sub-properties and a formatted() invokable.
 *
 * In QML the duration is not a number; use the \l milliseconds or
 * \l seconds sub-properties for arithmetic.
 */
class DRAGONMEDIABACKEND_EXPORT DragonDuration
{
    Q_GADGET
    QML_VALUE_TYPE(dragonDuration)
    Q_PROPERTY(qint64 milliseconds READ toMilliseconds WRITE setMilliseconds)
    Q_PROPERTY(qreal seconds READ toSeconds WRITE setSeconds)
    Q_PROPERTY(bool valid READ isValid CONSTANT)

public:
    /*!
     * Constructs an invalid duration; milliseconds() reports 0.
     */
    DragonDuration() = default;

    /*!
     * Converts \a duration. An implicit constructor so the chrono-based
     * getters of a class can serve a DragonDuration Q_PROPERTY directly.
     */
    DragonDuration(std::chrono::milliseconds duration); // implicit

    /*!
     * Converts the optional \a duration. An empty optional produces an
     * invalid duration whose milliseconds() is 0.
     */
    DragonDuration(std::optional<std::chrono::milliseconds> duration); // implicit

    /*!
     * Returns the duration in milliseconds, or 0 if it is invalid.
     */
    [[nodiscard]] qint64 toMilliseconds() const;

    /*!
     * Sets the duration to \a milliseconds, making it valid.
     */
    void setMilliseconds(qint64 milliseconds);

    /*!
     * Returns the duration in fractional seconds.
     */
    [[nodiscard]] qreal toSeconds() const;

    /*!
     * Sets the duration to \a seconds, making it valid.
     */
    void setSeconds(qreal seconds);

    /*!
     * Returns whether the duration is known.
     */
    [[nodiscard]] bool isValid() const;

    /*!
     * Returns the duration as std::chrono::milliseconds, 0 if invalid.
     * An implicit conversion so the chrono-based setters of a class can
     * serve a DragonDuration Q_PROPERTY directly.
     */
    [[nodiscard]] operator std::chrono::milliseconds() const; // implicit

    /*!
     * Returns the duration localized as "m:ss" below one hour or
     * "h:mm:ss" at or above it, or "--:--" when the duration is invalid.
     * The formatting is done by KFormat::formatDuration().
     */
    Q_INVOKABLE [[nodiscard]] QString formatted() const;

    [[nodiscard]] friend bool operator==(const DragonDuration &lhs, const DragonDuration &rhs) = default;

private:
    std::optional<std::chrono::milliseconds> m_duration;
};
