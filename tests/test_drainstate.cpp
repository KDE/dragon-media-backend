/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "sink/dragondrainstate.h"

class TestDrainState : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void startNewEpoch_table();
    void startNewEpoch_table_data();
    void claimEligibility();
    void claimIsCurrentSemantics();
    void doubleNotify();
    void staleEventAfterReset();
};

void TestDrainState::startNewEpoch_table_data()
{
    QTest::addColumn<bool>("notifyFirst");
    QTest::addColumn<bool>("claimFirst");

    QTest::newRow("clean") << false << false;
    QTest::newRow("decodeFinishedOnly") << true << false;
    QTest::newRow("claimed") << true << true;
    QTest::newRow("drainInitiatedWithoutNotify") << false << true;
}

void TestDrainState::startNewEpoch_table()
{
    QFETCH(bool, notifyFirst);
    QFETCH(bool, claimFirst);

    DragonDrainState state;
    if (notifyFirst) {
        state.notifyDecodeFinished();
    }
    if (claimFirst) {
        QCOMPARE(state.tryClaimDrain(), notifyFirst);
    }

    const auto epochBefore = state.epoch();
    state.startNewEpoch();

    QCOMPARE(state.epoch(), epochBefore + 1);
    QCOMPARE(state.decodeFinished(), false);
    QCOMPARE(state.claimIsCurrent(), false);
}

void TestDrainState::claimEligibility()
{
    DragonDrainState state;

    QCOMPARE(state.tryClaimDrain(), false);

    state.notifyDecodeFinished();
    QCOMPARE(state.tryClaimDrain(), true);

    QCOMPARE(state.tryClaimDrain(), false);

    state.startNewEpoch();
    state.notifyDecodeFinished();
    QCOMPARE(state.tryClaimDrain(), true);
    QCOMPARE(state.tryClaimDrain(), false);
}

void TestDrainState::claimIsCurrentSemantics()
{
    DragonDrainState state;
    state.notifyDecodeFinished();
    QVERIFY(state.tryClaimDrain());

    const auto claimed = state.claimedEpoch();
    QVERIFY(state.claimIsCurrent());
    QVERIFY(state.claimIsCurrent(claimed));

    state.consumeEmission();
    QCOMPARE(state.claimIsCurrent(), false);
    QCOMPARE(state.claimIsCurrent(claimed), false);

    DragonDrainState other;
    other.notifyDecodeFinished();
    QVERIFY(other.tryClaimDrain());
    const auto otherClaim = other.claimedEpoch();
    QVERIFY(other.claimIsCurrent(otherClaim));
    other.startNewEpoch();
    QCOMPARE(other.claimIsCurrent(), false);
    QCOMPARE(other.claimIsCurrent(otherClaim), false);
}

void TestDrainState::doubleNotify()
{
    DragonDrainState state;
    state.notifyDecodeFinished();
    const auto epoch = state.epoch();
    state.notifyDecodeFinished();

    QCOMPARE(state.epoch(), epoch);
    QCOMPARE(state.decodeFinished(), true);

    QVERIFY(state.tryClaimDrain());
    QCOMPARE(state.tryClaimDrain(), false);
}

void TestDrainState::staleEventAfterReset()
{
    DragonDrainState state;
    state.notifyDecodeFinished();
    QVERIFY(state.tryClaimDrain());

    state.startNewEpoch();
    QCOMPARE(state.claimIsCurrent(), false);
    QCOMPARE(state.claimIsCurrent(state.claimedEpoch()), false);
    QCOMPARE(state.epoch(), state.claimedEpoch() + 1);
}

QTEST_MAIN(TestDrainState)
#include "test_drainstate.moc"
