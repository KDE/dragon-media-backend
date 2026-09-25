/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "player/dragonpositionestimator.h"

#include <QSignalSpy>

#include <chrono>
#include <optional>

using namespace std::chrono_literals;

namespace
{
struct FakeDevice {
    std::optional<std::chrono::milliseconds> position;
};

class EstimatorHarness
{
public:
    explicit EstimatorHarness(std::optional<std::chrono::milliseconds> devicePosition = std::chrono::milliseconds{0})
    {
        estimator.setMonotonicClock([this]() {
            return now;
        });
        estimator.setDevicePositionCallback([this]() {
            return device.position;
        });
        device.position = devicePosition;
    }

    EstimatorHarness(const EstimatorHarness &) = delete;
    EstimatorHarness &operator=(const EstimatorHarness &) = delete;
    EstimatorHarness(EstimatorHarness &&) = delete;
    EstimatorHarness &operator=(EstimatorHarness &&) = delete;

    DragonPositionEstimator estimator;
    std::chrono::milliseconds now{0};
    FakeDevice device;
};
}

class TestPositionEstimator : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFrozenByDefault();
    void testStartAnchorsToDeviceOnFirstTick();
    void testStartWithoutDeviceFallsBackToLastPosition();
    void testExtrapolatesBetweenSyncPoints();
    void testNoReanchorWithinTolerance();
    void testReanchorsWhenDriftExceedsTolerance();
    void testFreezeHoldsPositionAndStopsTicking();
    void testSeekReportsTargetImmediatelyAndReanchors();
    void testSeekClampsToDuration();
    void testSeekWhileFrozenDoesNotAdvance();
    void testTrackChangedResetsToZeroWithNewDuration();
    void testSnapToDurationIsSticky();
    void testPositionClampedToDuration();
    void testResetPositionIsSilent();
    void testNoReanchorBeforeSyncIntervalElapses();
    void testDoubleFreezeKeepsPosition();
    void testSeekWithUnknownDurationClampsToZero();
    void testNoDurationClampDuringExtrapolation();
    void testDoubleStartReanchorsToDevice();
    void testDurationBecomesKnownMidPlayback();
    void testPositionProperty();
};

void TestPositionEstimator::testFrozenByDefault()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.now = 5000ms;
    h.estimator.tick();

    QCOMPARE(h.estimator.position(), 0ms);
    QCOMPARE(spy.count(), 0);
}

void TestPositionEstimator::testStartAnchorsToDeviceOnFirstTick()
{
    EstimatorHarness h{std::chrono::milliseconds{1234}};

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1234ms);

    h.now += 100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1334ms);
}

void TestPositionEstimator::testStartWithoutDeviceFallsBackToLastPosition()
{
    EstimatorHarness h{std::nullopt};

    h.estimator.resetPosition(700ms);
    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 700ms);

    h.now += 50ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 750ms);
}

void TestPositionEstimator::testExtrapolatesBetweenSyncPoints()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.device.position = 0ms;
    for (auto t = 50ms; t <= 450ms; t += 50ms) {
        h.now = t;
        h.estimator.tick();
        QCOMPARE(h.estimator.position(), t);
    }
}

void TestPositionEstimator::testNoReanchorWithinTolerance()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();

    h.now = 500ms;
    h.device.position = 400ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 500ms);

    h.now = 1000ms;
    h.device.position = 880ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1000ms);
    QCOMPARE(spy.count(), 3);
}

void TestPositionEstimator::testReanchorsWhenDriftExceedsTolerance()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.now = 500ms;
    h.device.position = 100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100ms);

    h.now = 600ms;
    h.device.position = 200ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 200ms);
}

void TestPositionEstimator::testFreezeHoldsPositionAndStopsTicking()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    h.now = 300ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300ms);

    h.estimator.freeze();
    QCOMPARE(h.estimator.position(), 300ms);

    h.now = 10000ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300ms);
    QCOMPARE(spy.count(), 2);

    h.device.position = 300ms;
    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300ms);
    h.now = 10100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 400ms);
}

void TestPositionEstimator::testSeekReportsTargetImmediatelyAndReanchors()
{
    EstimatorHarness h{std::nullopt};
    h.estimator.setDuration(3000ms);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(1000ms);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<std::chrono::milliseconds>(), 1000ms);
    QCOMPARE(h.estimator.position(), 1000ms);

    h.estimator.start();
    h.estimator.tick();
    h.now = 200ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1200ms);

    h.now = 500ms;
    h.device.position = 1050ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1050ms);
}

void TestPositionEstimator::testSeekClampsToDuration()
{
    EstimatorHarness h;
    h.estimator.setDuration(3000ms);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(5000ms);
    QCOMPARE(h.estimator.position(), 3000ms);
    QCOMPARE(spy.last().at(0).value<std::chrono::milliseconds>(), 3000ms);

    h.estimator.seek(-100ms);
    QCOMPARE(h.estimator.position(), 0ms);
    QCOMPARE(spy.last().at(0).value<std::chrono::milliseconds>(), 0ms);
    QCOMPARE(spy.count(), 2);
}

void TestPositionEstimator::testSeekWhileFrozenDoesNotAdvance()
{
    EstimatorHarness h{std::nullopt};
    h.estimator.setDuration(3000ms);

    h.estimator.seek(1500ms);
    h.now = 1000ms;
    h.estimator.tick();

    QCOMPARE(h.estimator.position(), 1500ms);
}

void TestPositionEstimator::testTrackChangedResetsToZeroWithNewDuration()
{
    EstimatorHarness h{std::chrono::milliseconds{2500}};
    h.estimator.setDuration(3000ms);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2500ms);

    h.estimator.trackChanged(4000ms);
    QCOMPARE(h.estimator.position(), 0ms);
    QCOMPARE(spy.last().at(0).value<std::chrono::milliseconds>(), 0ms);

    h.device.position = 0ms;
    h.now = 100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100ms);

    h.now = 5000ms;
    h.device.position = 4500ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 4000ms);
}

void TestPositionEstimator::testSnapToDurationIsSticky()
{
    EstimatorHarness h{std::chrono::milliseconds{2990}};
    h.estimator.setDuration(3000ms);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2990ms);

    h.estimator.snapToDuration();
    QCOMPARE(h.estimator.position(), 3000ms);
    QCOMPARE(spy.last().at(0).value<std::chrono::milliseconds>(), 3000ms);

    h.now = 700ms;
    h.device.position = 3000ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 3000ms);
}

void TestPositionEstimator::testPositionClampedToDuration()
{
    EstimatorHarness h{std::chrono::milliseconds{900}};
    h.estimator.setDuration(1000ms);

    h.estimator.start();
    h.estimator.tick();

    h.now = 300ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1000ms);

    h.now = 400ms;
    QCOMPARE(h.estimator.position(), 1000ms);
}

void TestPositionEstimator::testResetPositionIsSilent()
{
    EstimatorHarness h{std::chrono::milliseconds{1000}};
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(spy.count(), 1);

    h.estimator.resetPosition(0ms);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(h.estimator.position(), 0ms);

    h.device.position = 0ms;
    h.now = 100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 0ms);
}

void TestPositionEstimator::testNoReanchorBeforeSyncIntervalElapses()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.now = 100ms;
    h.device.position = 5000ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100ms);

    h.now = 499ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 499ms);

    h.now = 500ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5000ms);

    h.now = 600ms;
    h.device.position = 5100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5100ms);
}

void TestPositionEstimator::testDoubleFreezeKeepsPosition()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    h.now = 300ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300ms);

    h.estimator.freeze();
    h.estimator.freeze();

    h.now = 5000ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300ms);
    QCOMPARE(spy.count(), 2);
}

void TestPositionEstimator::testSeekWithUnknownDurationClampsToZero()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(1000ms);
    QCOMPARE(h.estimator.position(), 0ms);
    QCOMPARE(spy.last().at(0).value<std::chrono::milliseconds>(), 0ms);

    h.estimator.setDuration(0ms);
    h.estimator.seek(500ms);
    QCOMPARE(h.estimator.position(), 0ms);
}

void TestPositionEstimator::testNoDurationClampDuringExtrapolation()
{
    EstimatorHarness h{std::chrono::milliseconds{2000}};
    h.estimator.setDuration(0ms);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2000ms);

    h.device.position = std::nullopt;
    h.now = 1000ms;
    h.estimator.tick();
    QVERIFY2(h.estimator.position() > 2000ms, "Streams with unknown duration must not clamp the extrapolated position");
    QCOMPARE(h.estimator.position(), 3000ms);
}

void TestPositionEstimator::testDoubleStartReanchorsToDevice()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();
    h.now = 100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100ms);

    h.device.position = 600ms;
    h.estimator.start();
    QCOMPARE(h.estimator.position(), 100ms);

    h.now = 150ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 600ms);
}

void TestPositionEstimator::testDurationBecomesKnownMidPlayback()
{
    EstimatorHarness h{std::nullopt};

    h.estimator.start();
    h.estimator.tick();

    h.now = 5000ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5000ms);

    h.estimator.setDuration(4000ms);
    QCOMPARE(h.estimator.position(), 4000ms);

    h.now = 5100ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 4000ms);

    h.estimator.setDuration(6000ms);
    h.now = 5200ms;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5200ms);
}

void TestPositionEstimator::testPositionProperty()
{
    EstimatorHarness h{std::nullopt};

    QCOMPARE(h.estimator.property("position").value<std::chrono::milliseconds>(), 0ms);
    h.estimator.setDuration(1000ms);
    h.estimator.seek(420ms);
    QCOMPARE(h.estimator.property("position").value<std::chrono::milliseconds>(), 420ms);
}

QTEST_GUILESS_MAIN(TestPositionEstimator)
#include "test_positionestimator.moc"
