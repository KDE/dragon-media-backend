/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Unit tests for DragonCompletion point-to-point coroutine awaitable.
 */

#include <QCoreApplication>
#include <QCoroTask>
#include <QThread>
#include <QtTest>

#include "player/dragoncompletion.h"

#include <thread>

using namespace DragonMultimedia;

class TestCompletion : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testBasicSetResult();
    void testCancel();
    void testFirstCallWins();
    void testSetResultBeforeAwait();
    void testSetResultFromOtherThread();
    void testCancelFromOtherThread();
};

struct CompletionCapture {
    InitResult result;
    bool done = false;
    Qt::HANDLE resumeThreadId = nullptr;
};

static QCoro::Task<void> captureAwait(std::shared_ptr<DragonCompletion> c, CompletionCapture *cap)
{
    cap->result = co_await *c;
    cap->resumeThreadId = QThread::currentThreadId();
    cap->done = true;
}

static void waitForDone(CompletionCapture *cap)
{
    for (int i = 0; i < 200 && !cap->done; ++i) {
        QTest::qWait(10);
    }
}

void TestCompletion::testBasicSetResult()
{
    CompletionCapture cap;
    auto c = std::make_shared<DragonCompletion>();

    auto task = captureAwait(c, &cap);

    InitResult r;
    r.success = true;
    r.sampleRate = 44100;
    r.channels = 2;
    QVERIFY(c->setResult(r));

    waitForDone(&cap);

    QVERIFY(cap.done);
    QVERIFY(cap.result.success);
    QCOMPARE(cap.result.sampleRate, 44100);
    QCOMPARE(cap.result.channels, 2);
    QVERIFY(!cap.result.cancelled);
}

void TestCompletion::testCancel()
{
    CompletionCapture cap;
    auto c = std::make_shared<DragonCompletion>();

    auto task = captureAwait(c, &cap);

    QVERIFY(c->cancel(QStringLiteral("Test cancellation")));

    waitForDone(&cap);

    QVERIFY(cap.done);
    QVERIFY(!cap.result.success);
    QVERIFY(cap.result.cancelled);
    QCOMPARE(cap.result.errorMessage, QStringLiteral("Test cancellation"));
}

void TestCompletion::testFirstCallWins()
{
    {
        CompletionCapture cap;
        auto c = std::make_shared<DragonCompletion>();
        auto task = captureAwait(c, &cap);

        InitResult r;
        r.success = true;
        QVERIFY(c->setResult(r));
        QVERIFY(!c->cancel(QStringLiteral("ignored")));

        waitForDone(&cap);
        QVERIFY(cap.result.success);
        QVERIFY(!cap.result.cancelled);
    }

    {
        CompletionCapture cap;
        auto c = std::make_shared<DragonCompletion>();
        auto task = captureAwait(c, &cap);

        QVERIFY(c->cancel(QStringLiteral("first")));
        InitResult r;
        r.success = true;
        QVERIFY(!c->setResult(r));

        waitForDone(&cap);
        QVERIFY(!cap.result.success);
        QVERIFY(cap.result.cancelled);
        QCOMPARE(cap.result.errorMessage, QStringLiteral("first"));
    }
}

void TestCompletion::testSetResultBeforeAwait()
{
    CompletionCapture cap;
    auto c = std::make_shared<DragonCompletion>();

    InitResult r;
    r.success = true;
    r.sampleRate = 48000;
    QVERIFY(c->setResult(r));

    auto task = captureAwait(c, &cap);

    waitForDone(&cap);

    QVERIFY(cap.done);
    QVERIFY(cap.result.success);
    QCOMPARE(cap.result.sampleRate, 48000);
}

void TestCompletion::testSetResultFromOtherThread()
{
    CompletionCapture cap;
    auto c = std::make_shared<DragonCompletion>();
    Qt::HANDLE mainThreadId = QThread::currentThreadId();

    auto task = captureAwait(c, &cap);

    std::thread worker([c]() {
        InitResult r;
        r.success = true;
        r.sampleRate = 48000;
        c->setResult(r);
    });
    worker.join();

    waitForDone(&cap);

    QVERIFY(cap.done);
    QVERIFY(cap.result.success);
    QCOMPARE(cap.resumeThreadId, mainThreadId);
}

void TestCompletion::testCancelFromOtherThread()
{
    CompletionCapture cap;
    auto c = std::make_shared<DragonCompletion>();
    Qt::HANDLE mainThreadId = QThread::currentThreadId();

    auto task = captureAwait(c, &cap);

    std::thread worker([c]() {
        c->cancel(QStringLiteral("Cancelled from worker"));
    });
    worker.join();

    waitForDone(&cap);

    QVERIFY(cap.done);
    QVERIFY(!cap.result.success);
    QVERIFY(cap.result.cancelled);
    QCOMPARE(cap.resumeThreadId, mainThreadId);
}

QTEST_MAIN(TestCompletion)
#include "test_completion.moc"
