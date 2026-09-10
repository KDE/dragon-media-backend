/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "player/dragonpositionestimator.h"

#include <QSignalSpy>

#include <optional>

namespace
{
struct FakeDevice {
    std::optional<qint64> position;
};

class EstimatorHarness
{
public:
    explicit EstimatorHarness(std::optional<qint64> devicePosition = qint64{0})
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
    qint64 now = 0;
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

    h.now = 5000;
    h.estimator.tick();

    QCOMPARE(h.estimator.position(), 0LL);
    QCOMPARE(spy.count(), 0);
}

void TestPositionEstimator::testStartAnchorsToDeviceOnFirstTick()
{
    EstimatorHarness h{1234};

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1234LL);

    h.now += 100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1334LL);
}

void TestPositionEstimator::testStartWithoutDeviceFallsBackToLastPosition()
{
    EstimatorHarness h{std::nullopt};

    h.estimator.resetPosition(700);
    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 700LL);

    h.now += 50;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 750LL);
}

void TestPositionEstimator::testExtrapolatesBetweenSyncPoints()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.device.position = 0;
    for (qint64 t = 50; t <= 450; t += 50) {
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

    h.now = 500;
    h.device.position = 400;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 500LL);

    h.now = 1000;
    h.device.position = 880;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1000LL);
    QCOMPARE(spy.count(), 3);
}

void TestPositionEstimator::testReanchorsWhenDriftExceedsTolerance()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.now = 500;
    h.device.position = 100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100LL);

    h.now = 600;
    h.device.position = 200;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 200LL);
}

void TestPositionEstimator::testFreezeHoldsPositionAndStopsTicking()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    h.now = 300;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300LL);

    h.estimator.freeze();
    QCOMPARE(h.estimator.position(), 300LL);

    h.now = 10000;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300LL);
    QCOMPARE(spy.count(), 2);

    h.device.position = 300;
    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300LL);
    h.now = 10100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 400LL);
}

void TestPositionEstimator::testSeekReportsTargetImmediatelyAndReanchors()
{
    EstimatorHarness h{std::nullopt};
    h.estimator.setDuration(3000);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(1000);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toLongLong(), 1000LL);
    QCOMPARE(h.estimator.position(), 1000LL);

    h.estimator.start();
    h.estimator.tick();
    h.now = 200;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1200LL);

    h.now = 500;
    h.device.position = 1050;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1050LL);
}

void TestPositionEstimator::testSeekClampsToDuration()
{
    EstimatorHarness h;
    h.estimator.setDuration(3000);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(5000);
    QCOMPARE(h.estimator.position(), 3000LL);
    QCOMPARE(spy.last().at(0).toLongLong(), 3000LL);

    h.estimator.seek(-100);
    QCOMPARE(h.estimator.position(), 0LL);
    QCOMPARE(spy.last().at(0).toLongLong(), 0LL);
    QCOMPARE(spy.count(), 2);
}

void TestPositionEstimator::testSeekWhileFrozenDoesNotAdvance()
{
    EstimatorHarness h{std::nullopt};
    h.estimator.setDuration(3000);

    h.estimator.seek(1500);
    h.now = 1000;
    h.estimator.tick();

    QCOMPARE(h.estimator.position(), 1500LL);
}

void TestPositionEstimator::testTrackChangedResetsToZeroWithNewDuration()
{
    EstimatorHarness h{2500};
    h.estimator.setDuration(3000);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2500LL);

    h.estimator.trackChanged(4000);
    QCOMPARE(h.estimator.position(), 0LL);
    QCOMPARE(spy.last().at(0).toLongLong(), 0LL);

    h.device.position = 0;
    h.now = 100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100LL);

    h.now = 5000;
    h.device.position = 4500;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 4000LL);
}

void TestPositionEstimator::testSnapToDurationIsSticky()
{
    EstimatorHarness h{2990};
    h.estimator.setDuration(3000);
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2990LL);

    h.estimator.snapToDuration();
    QCOMPARE(h.estimator.position(), 3000LL);
    QCOMPARE(spy.last().at(0).toLongLong(), 3000LL);

    h.now = 700;
    h.device.position = 3000;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 3000LL);
}

void TestPositionEstimator::testPositionClampedToDuration()
{
    EstimatorHarness h{900};
    h.estimator.setDuration(1000);

    h.estimator.start();
    h.estimator.tick();

    h.now = 300;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 1000LL);

    h.now = 400;
    QCOMPARE(h.estimator.position(), 1000LL);
}

void TestPositionEstimator::testResetPositionIsSilent()
{
    EstimatorHarness h{1000};
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(spy.count(), 1);

    h.estimator.resetPosition(0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(h.estimator.position(), 0LL);

    h.device.position = 0;
    h.now = 100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 0LL);
}

void TestPositionEstimator::testNoReanchorBeforeSyncIntervalElapses()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();

    h.now = 100;
    h.device.position = 5000;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100LL);

    h.now = 499;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 499LL);

    h.now = 500;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5000LL);

    h.now = 600;
    h.device.position = 5100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5100LL);
}

void TestPositionEstimator::testDoubleFreezeKeepsPosition()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.start();
    h.estimator.tick();
    h.now = 300;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300LL);

    h.estimator.freeze();
    h.estimator.freeze();

    h.now = 5000;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 300LL);
    QCOMPARE(spy.count(), 2);
}

void TestPositionEstimator::testSeekWithUnknownDurationClampsToZero()
{
    EstimatorHarness h;
    QSignalSpy spy(&h.estimator, &DragonPositionEstimator::positionChanged);

    h.estimator.seek(1000);
    QCOMPARE(h.estimator.position(), 0LL);
    QCOMPARE(spy.last().at(0).toLongLong(), 0LL);

    h.estimator.setDuration(0);
    h.estimator.seek(500);
    QCOMPARE(h.estimator.position(), 0LL);
}

void TestPositionEstimator::testNoDurationClampDuringExtrapolation()
{
    EstimatorHarness h{2000};
    h.estimator.setDuration(0);

    h.estimator.start();
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 2000LL);

    h.device.position = std::nullopt;
    h.now = 1000;
    h.estimator.tick();
    QVERIFY2(h.estimator.position() > 2000LL, "Streams with unknown duration must not clamp the extrapolated position");
    QCOMPARE(h.estimator.position(), 3000LL);
}

void TestPositionEstimator::testDoubleStartReanchorsToDevice()
{
    EstimatorHarness h;

    h.estimator.start();
    h.estimator.tick();
    h.now = 100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 100LL);

    h.device.position = 600;
    h.estimator.start();
    QCOMPARE(h.estimator.position(), 100LL);

    h.now = 150;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 600LL);
}

void TestPositionEstimator::testDurationBecomesKnownMidPlayback()
{
    EstimatorHarness h{std::nullopt};

    h.estimator.start();
    h.estimator.tick();

    h.now = 5000;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5000LL);

    h.estimator.setDuration(4000);
    QCOMPARE(h.estimator.position(), 4000LL);

    h.now = 5100;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 4000LL);

    h.estimator.setDuration(6000);
    h.now = 5200;
    h.estimator.tick();
    QCOMPARE(h.estimator.position(), 5200LL);
}

void TestPositionEstimator::testPositionProperty()
{
    EstimatorHarness h{std::nullopt};

    QCOMPARE(h.estimator.property("position").toLongLong(), 0LL);
    h.estimator.setDuration(1000);
    h.estimator.seek(420);
    QCOMPARE(h.estimator.property("position").toLongLong(), 420LL);
}

QTEST_GUILESS_MAIN(TestPositionEstimator)
#include "test_positionestimator.moc"
