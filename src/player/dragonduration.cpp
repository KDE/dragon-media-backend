/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonduration.h"

#include <KFormat>

DragonDuration::DragonDuration(std::chrono::milliseconds duration)
    : m_duration(duration)
{
}

DragonDuration::DragonDuration(std::optional<std::chrono::milliseconds> duration)
    : m_duration(duration)
{
}

qint64 DragonDuration::toMilliseconds() const
{
    return m_duration.value_or(std::chrono::milliseconds{0}).count();
}

void DragonDuration::setMilliseconds(qint64 milliseconds)
{
    m_duration = std::chrono::milliseconds{milliseconds};
}

qreal DragonDuration::toSeconds() const
{
    return static_cast<qreal>(toMilliseconds()) / 1000.0;
}

void DragonDuration::setSeconds(qreal seconds)
{
    m_duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<qreal>{seconds});
}

bool DragonDuration::isValid() const
{
    return m_duration.has_value();
}

DragonDuration::operator std::chrono::milliseconds() const
{
    return m_duration.value_or(std::chrono::milliseconds{0});
}

QString DragonDuration::formatted() const
{
    if (!m_duration) {
        return QStringLiteral("--:--");
    }
    // KFormat always prints the hours with DefaultDuration and folds them
    // into the minutes with FoldHours; switching at the hour mark yields
    // "3:41" below one hour and "1:01:01" at or above it.
    const KFormat format;
    const auto options = *m_duration >= std::chrono::hours{1} ? KFormat::DefaultDuration : KFormat::FoldHours;
    return format.formatDuration(static_cast<quint64>(toMilliseconds()), options);
}
