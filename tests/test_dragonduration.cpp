/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Unit tests for DragonDuration and its use as the DragonPlayer property
 * serialization type.
 */

#include <QtTest>

#include "logging_timestamp_init.h"

#include "player/dragonduration.h"
#include <DragonMediaBackend/dragonplayer.h>

#include <QSignalSpy>
#include <QVariant>

#include <chrono>
#include <optional>

using namespace std::chrono_literals;

class TestDragonDuration : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDefaultIsInvalid();
    void testFromDuration();
    void testFromOptional();
    void testConversionRoundTrip();
    void testInvalidConvertsToZero();
    void testFormatted();
    void testEquality();

    void testPlayerPropertyRead();
    void testPlayerPropertyWrite();
    void testPlayerDurationPropertyInvalidWithoutSource();
};

void TestDragonDuration::testDefaultIsInvalid()
{
    const DragonDuration duration;
    QVERIFY(!duration.isValid());
    QCOMPARE(duration.toMilliseconds(), qint64(0));
    QCOMPARE(duration.formatted(), QStringLiteral("--:--"));
}

void TestDragonDuration::testFromDuration()
{
    const DragonDuration duration{std::chrono::milliseconds{221'000}};
    QVERIFY(duration.isValid());
    QCOMPARE(duration.toMilliseconds(), qint64(221'000));
    QCOMPARE(duration.toSeconds(), qreal(221.0));
}

void TestDragonDuration::testFromOptional()
{
    const DragonDuration fromEmpty{std::optional<std::chrono::milliseconds>{}};
    QVERIFY(!fromEmpty.isValid());
    QCOMPARE(fromEmpty.toMilliseconds(), qint64(0));

    const DragonDuration fromValue{std::optional<std::chrono::milliseconds>{5'000ms}};
    QVERIFY(fromValue.isValid());
    QCOMPARE(fromValue.toMilliseconds(), qint64(5'000));
}

void TestDragonDuration::testConversionRoundTrip()
{
    const std::chrono::milliseconds original{123'456ms};
    const DragonDuration duration{original};
    const std::chrono::milliseconds converted{duration};
    QCOMPARE(converted, original);
}

void TestDragonDuration::testInvalidConvertsToZero()
{
    const DragonDuration duration;
    QCOMPARE(static_cast<std::chrono::milliseconds>(duration), 0ms);
}

void TestDragonDuration::testFormatted()
{
    QCOMPARE(DragonDuration{}.formatted(), QStringLiteral("--:--"));
    QCOMPARE(DragonDuration{0ms}.formatted(), QStringLiteral("0:00"));
    QCOMPARE(DragonDuration{5'000ms}.formatted(), QStringLiteral("0:05"));
    QCOMPARE(DragonDuration{221'000ms}.formatted(), QStringLiteral("3:41"));
    QCOMPARE(DragonDuration{3'661'000ms}.formatted(), QStringLiteral("1:01:01"));
}

void TestDragonDuration::testEquality()
{
    QCOMPARE(DragonDuration{}, DragonDuration{std::optional<std::chrono::milliseconds>{}});
    QCOMPARE(DragonDuration{100ms}, DragonDuration{100ms});
    QVERIFY(DragonDuration{100ms} != DragonDuration{200ms});
}

void TestDragonDuration::testPlayerPropertyRead()
{
    DragonPlayer player;
    const QVariant value = player.property("prefinishMark");
    QVERIFY(value.isValid());
    QCOMPARE(value.metaType(), QMetaType::fromType<DragonDuration>());
    QCOMPARE(value.value<DragonDuration>(), DragonDuration{2000ms});

    const QVariant position = player.property("position");
    QVERIFY(position.isValid());
    QCOMPARE(position.value<DragonDuration>(), DragonDuration{0ms});

    QVERIFY(QMetaType::fromType<DragonDuration>().flags() & QMetaType::IsGadget);
}

void TestDragonDuration::testPlayerPropertyWrite()
{
    DragonPlayer player;
    QSignalSpy spy(&player, &DragonPlayer::prefinishMarkChanged);

    const bool written = player.setProperty("prefinishMark", QVariant::fromValue(DragonDuration{250ms}));
    QVERIFY(written);
    QCOMPARE(player.prefinishMark(), 250ms);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).value<std::chrono::milliseconds>(), 250ms);

    // Setting the same value again must not re-notify.
    QVERIFY(player.setProperty("prefinishMark", QVariant::fromValue(DragonDuration{250ms})));
    QCOMPARE(spy.count(), 1);
}

void TestDragonDuration::testPlayerDurationPropertyInvalidWithoutSource()
{
    DragonPlayer player;
    const QVariant duration = player.property("duration");
    QVERIFY(duration.isValid());
    QVERIFY(!duration.value<DragonDuration>().isValid());
    QCOMPARE(duration.value<DragonDuration>().toMilliseconds(), qint64(0));
}

QTEST_MAIN(TestDragonDuration)
#include "test_dragonduration.moc"
