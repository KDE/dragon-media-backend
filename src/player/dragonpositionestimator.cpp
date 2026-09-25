/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpositionestimator.h"

#include <dragonmediabackend_logging.h>

#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

namespace
{
constexpr std::chrono::milliseconds kPositionSyncInterval{500};
constexpr std::chrono::milliseconds kPositionSyncTolerance{150};
constexpr std::chrono::milliseconds kTickInterval{50};
}

DragonPositionEstimator::DragonPositionEstimator(QObject *parent)
    : QObject(parent)
    , m_tickTimer(new QTimer(this))
{
    m_clock.start();
    m_tickTimer->setInterval(kTickInterval);
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

void DragonPositionEstimator::setDuration(std::optional<std::chrono::milliseconds> duration)
{
    m_duration = duration;
}

std::chrono::milliseconds DragonPositionEstimator::position() const
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

void DragonPositionEstimator::seek(std::chrono::milliseconds target)
{
    const auto upperBound = std::max(m_duration.value_or(std::chrono::milliseconds{0}), std::chrono::milliseconds{0});
    target = std::clamp(target, std::chrono::milliseconds{0}, upperBound);
    m_position = target;
    m_anchorValid = false;
    Q_EMIT positionChanged(m_position);
}

void DragonPositionEstimator::trackChanged(std::optional<std::chrono::milliseconds> duration)
{
    m_duration = duration;
    m_position = {};
    anchorAt(std::chrono::milliseconds{0});
    Q_EMIT positionChanged(std::chrono::milliseconds{0});
}

void DragonPositionEstimator::snapToDuration()
{
    if (!m_duration || *m_duration <= std::chrono::milliseconds{0}) {
        return;
    }
    anchorAt(*m_duration);
    m_position = *m_duration;
    Q_EMIT positionChanged(m_position);
}

void DragonPositionEstimator::resetPosition(std::chrono::milliseconds position)
{
    m_position = position;
    m_anchorValid = false;
}

void DragonPositionEstimator::tick()
{
    if (!m_tickTimer->isActive()) {
        return;
    }

    const std::optional<std::chrono::milliseconds> devicePos = m_devicePosition ? m_devicePosition() : std::nullopt;
    if (!m_anchorValid) {
        anchorAt(devicePos.value_or(m_position));
    } else if (devicePos && elapsed() - m_lastSyncElapsed >= kPositionSyncInterval) {
        const std::chrono::milliseconds extrapolatedPos = m_anchorPosition + (elapsed() - m_anchorElapsed);
        const std::chrono::milliseconds drift = *devicePos - extrapolatedPos;
        if (drift > kPositionSyncTolerance || drift < -kPositionSyncTolerance) {
            qCDebug(dragonMediaBackendPlayer) << "position re-anchored to device: extrapolated=" << extrapolatedPos << "device=" << *devicePos;
            anchorAt(*devicePos);
        } else {
            m_lastSyncElapsed = elapsed();
        }
    }

    const std::chrono::milliseconds pos = extrapolated();
    m_position = pos;
    Q_EMIT positionChanged(pos);
}

std::chrono::milliseconds DragonPositionEstimator::elapsed() const
{
    return m_monotonicClock ? m_monotonicClock() : std::chrono::milliseconds{m_clock.elapsed()};
}

std::chrono::milliseconds DragonPositionEstimator::extrapolated() const
{
    if (!m_anchorValid) {
        return m_position;
    }
    const std::chrono::milliseconds pos = m_anchorPosition + (elapsed() - m_anchorElapsed);
    return (m_duration && *m_duration > std::chrono::milliseconds{0}) ? std::min(pos, *m_duration) : pos;
}

void DragonPositionEstimator::anchorAt(std::chrono::milliseconds position)
{
    m_anchorPosition = position;
    m_anchorElapsed = elapsed();
    m_lastSyncElapsed = m_anchorElapsed;
    m_anchorValid = true;
}
