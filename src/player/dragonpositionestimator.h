/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>

class DRAGONMEDIABACKEND_EXPORT DragonPositionEstimator : public QObject
{
    Q_OBJECT
    Q_PROPERTY(std::chrono::milliseconds position READ position NOTIFY positionChanged)

public:
    using DevicePositionCallback = std::function<std::optional<std::chrono::milliseconds>()>;

    using MonotonicClock = std::function<std::chrono::milliseconds()>;

    explicit DragonPositionEstimator(QObject *parent = nullptr);

    void setDevicePositionCallback(DevicePositionCallback callback);

    void setMonotonicClock(MonotonicClock clock);

    void setDuration(std::optional<std::chrono::milliseconds> duration);

    [[nodiscard]] std::chrono::milliseconds position() const;

public Q_SLOTS:
    void start();

    void freeze();

    void seek(std::chrono::milliseconds target);

    void trackChanged(std::optional<std::chrono::milliseconds> duration);

    void snapToDuration();

    void resetPosition(std::chrono::milliseconds position = {});

    void tick();

Q_SIGNALS:
    void positionChanged(std::chrono::milliseconds position);

private:
    [[nodiscard]] std::chrono::milliseconds elapsed() const;
    [[nodiscard]] std::chrono::milliseconds extrapolated() const;
    void anchorAt(std::chrono::milliseconds position);

    QTimer *m_tickTimer = nullptr;
    QElapsedTimer m_clock;
    MonotonicClock m_monotonicClock;
    DevicePositionCallback m_devicePosition;
    std::chrono::milliseconds m_position{};
    std::optional<std::chrono::milliseconds> m_duration{};
    std::chrono::milliseconds m_anchorPosition{};
    std::chrono::milliseconds m_anchorElapsed{};
    std::chrono::milliseconds m_lastSyncElapsed{};
    bool m_anchorValid = false;
};
