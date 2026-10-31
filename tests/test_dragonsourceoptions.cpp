/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include <DragonSourceOptions>

#include <chrono>

using namespace std::chrono_literals;

class TestDragonSourceOptions : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testDefaultConstruction();
    void testSetCrossfadeDuration();
    void testNegativeClamping();
    void testCopyOnWriteDetach();
    void testEquality();
};

void TestDragonSourceOptions::testDefaultConstruction()
{
    const DragonSourceOptions options;
    QCOMPARE(options.crossfadeDuration(), 0ms);
}

void TestDragonSourceOptions::testSetCrossfadeDuration()
{
    DragonSourceOptions options;
    options.setCrossfadeDuration(2s);
    QCOMPARE(options.crossfadeDuration(), 2000ms);
}

void TestDragonSourceOptions::testNegativeClamping()
{
    DragonSourceOptions options;
    options.setCrossfadeDuration(-500ms);
    QCOMPARE(options.crossfadeDuration(), 0ms);
}

void TestDragonSourceOptions::testCopyOnWriteDetach()
{
    DragonSourceOptions original;
    original.setCrossfadeDuration(1500ms);
    DragonSourceOptions copy = original;
    QCOMPARE(copy, original);

    copy.setCrossfadeDuration(500ms);
    QCOMPARE(original.crossfadeDuration(), 1500ms);
    QCOMPARE(copy.crossfadeDuration(), 500ms);
    QVERIFY(copy != original);
}

void TestDragonSourceOptions::testEquality()
{
    DragonSourceOptions a;
    a.setCrossfadeDuration(1s);
    DragonSourceOptions b;
    b.setCrossfadeDuration(1000ms);
    QCOMPARE(a, b);
    QVERIFY(!(a != b));

    DragonSourceOptions c;
    c.setCrossfadeDuration(2s);
    QVERIFY(a != c);
}

QTEST_GUILESS_MAIN(TestDragonSourceOptions)
#include "test_dragonsourceoptions.moc"
