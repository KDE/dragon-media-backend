/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <functional>
#include <optional>

class DRAGONMEDIABACKEND_EXPORT DragonPositionEstimator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)

public:
    using DevicePositionCallback = std::function<std::optional<qint64>()>;

    using MonotonicClock = std::function<qint64()>;

    explicit DragonPositionEstimator(QObject *parent = nullptr);

    void setDevicePositionCallback(DevicePositionCallback callback);

    void setMonotonicClock(MonotonicClock clock);

    void setDuration(qint64 durationMs);

    [[nodiscard]] qint64 position() const;

public Q_SLOTS:
    void start();

    void freeze();

    void seek(qint64 targetMs);

    void trackChanged(qint64 durationMs);

    void snapToDuration();

    void resetPosition(qint64 positionMs = 0);

    void tick();

Q_SIGNALS:
    void positionChanged(qint64 positionMs);

private:
    [[nodiscard]] qint64 elapsed() const;
    [[nodiscard]] qint64 extrapolated() const;
    void anchorAt(qint64 positionMs);

    QTimer *m_tickTimer = nullptr;
    QElapsedTimer m_clock;
    MonotonicClock m_monotonicClock;
    DevicePositionCallback m_devicePosition;
    qint64 m_position = 0;
    qint64 m_duration = -1;
    qint64 m_anchorPositionMs = 0;
    qint64 m_anchorElapsedMs = 0;
    qint64 m_lastSyncElapsedMs = 0;
    bool m_anchorValid = false;
};
