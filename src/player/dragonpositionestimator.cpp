/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpositionestimator.h"

#include <dragonmediabackend_logging.h>

#include <algorithm>

namespace
{
constexpr qint64 kPositionSyncIntervalMs = 500;
constexpr qint64 kPositionSyncToleranceMs = 150;
constexpr int kTickIntervalMs = 50;
}

DragonPositionEstimator::DragonPositionEstimator(QObject *parent)
    : QObject(parent)
    , m_tickTimer(new QTimer(this))
{
    m_clock.start();
    m_tickTimer->setInterval(kTickIntervalMs);
    m_tickTimer->setTimerType(Qt::PreciseTimer);
    connect(m_tickTimer, &QTimer::timeout, this, &DragonPositionEstimator::tick);
}

void DragonPositionEstimator::setDevicePositionCallback(DevicePositionCallback callback)
{
    m_devicePosition = std::move(callback);
}

void DragonPositionEstimator::setMonotonicClock(MonotonicClock clock)
{
    m_monotonicClock = std::move(clock);
}

void DragonPositionEstimator::setDuration(qint64 durationMs)
{
    m_duration = durationMs;
}

qint64 DragonPositionEstimator::position() const
{
    return extrapolated();
}

void DragonPositionEstimator::start()
{
    m_anchorValid = false;
    m_tickTimer->start();
}

void DragonPositionEstimator::freeze()
{
    if (m_anchorValid) {
        m_position = extrapolated();
        m_anchorValid = false;
    }
    m_tickTimer->stop();
}

void DragonPositionEstimator::seek(qint64 targetMs)
{
    targetMs = std::clamp(targetMs, qint64{0}, std::max(m_duration, qint64{0}));
    m_position = targetMs;
    m_anchorValid = false;
    Q_EMIT positionChanged(m_position);
}

void DragonPositionEstimator::trackChanged(qint64 durationMs)
{
    m_duration = durationMs;
    m_position = 0;
    anchorAt(0);
    Q_EMIT positionChanged(0);
}

void DragonPositionEstimator::snapToDuration()
{
    if (m_duration <= 0) {
        return;
    }
    anchorAt(m_duration);
    m_position = m_duration;
    Q_EMIT positionChanged(m_position);
}

void DragonPositionEstimator::resetPosition(qint64 positionMs)
{
    m_position = positionMs;
    m_anchorValid = false;
}

void DragonPositionEstimator::tick()
{
    if (!m_tickTimer->isActive()) {
        return;
    }

    const std::optional<qint64> devicePos = m_devicePosition ? m_devicePosition() : std::nullopt;
    if (!m_anchorValid) {
        anchorAt(devicePos.value_or(m_position));
    } else if (devicePos && elapsed() - m_lastSyncElapsedMs >= kPositionSyncIntervalMs) {
        const qint64 extrapolatedPos = m_anchorPositionMs + (elapsed() - m_anchorElapsedMs);
        const qint64 drift = *devicePos - extrapolatedPos;
        if (drift > kPositionSyncToleranceMs || drift < -kPositionSyncToleranceMs) {
            qCDebug(dragonMediaBackendPlayer) << "position re-anchored to device: extrapolated=" << extrapolatedPos << "device=" << *devicePos;
            anchorAt(*devicePos);
        } else {
            m_lastSyncElapsedMs = elapsed();
        }
    }

    const qint64 pos = extrapolated();
    m_position = pos;
    Q_EMIT positionChanged(pos);
}

qint64 DragonPositionEstimator::elapsed() const
{
    return m_monotonicClock ? m_monotonicClock() : m_clock.elapsed();
}

qint64 DragonPositionEstimator::extrapolated() const
{
    if (!m_anchorValid) {
        return m_position;
    }
    const qint64 pos = m_anchorPositionMs + (elapsed() - m_anchorElapsedMs);
    return m_duration > 0 ? std::min(pos, m_duration) : pos;
}

void DragonPositionEstimator::anchorAt(qint64 positionMs)
{
    m_anchorPositionMs = positionMs;
    m_anchorElapsedMs = elapsed();
    m_lastSyncElapsedMs = m_anchorElapsedMs;
    m_anchorValid = true;
}
