/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Unit tests for DragonCompletion signal-backed coroutine awaitable.
 */

#include <QCoreApplication>
#include <QCoroTask>
#include <QtTest>

#include "decoder/dragoncompletion.h"

#include <atomic>
#include <chrono>
#include <mutex>
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
    void testDestroyBeforeResumeFires();
    void testDestroyDuringSuspend();
    void testCancelUnderLockDeadlock();
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

// Raw-pointer variant: the coroutine frame does NOT hold a shared_ptr to the
// completion, so dropping the last external shared_ptr destroys the completion
// and severs the Qt connection (receiver-lifetime safety).
static QCoro::Task<void> captureAwaitRaw(DragonCompletion *c, CompletionCapture *cap)
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

    // Result is buffered; await should resolve synchronously.
    QVERIFY(c->isReady());
    QCOMPARE(c->result().sampleRate, 48000);

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
    QCOMPARE(cap.result.sampleRate, 48000);
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
    QCOMPARE(cap.result.errorMessage, QStringLiteral("Cancelled from worker"));
    QCOMPARE(cap.resumeThreadId, mainThreadId);
}

void TestCompletion::testDestroyBeforeResumeFires()
{
    CompletionCapture cap;

    auto c = std::make_shared<DragonCompletion>();
    auto task = captureAwait(c, &cap);

    InitResult r;
    r.success = true;
    r.sampleRate = 48000;
    QVERIFY(c->setResult(r));

    // Drop our last external reference before the queued event fires.
    c.reset();

    // Process events the awaiter's State holds a shared_ptr that keeps the
    // completion alive until the resume fires.
    waitForDone(&cap);
    QVERIFY(cap.done);
    QVERIFY(cap.result.success);
    QCOMPARE(cap.result.sampleRate, 48000);
}

void TestCompletion::testDestroyDuringSuspend()
{
    CompletionCapture cap;

    auto c = std::make_shared<DragonCompletion>();
    // Raw pointer: the coroutine frame must NOT hold a shared_ptr, otherwise
    // dropping `c` would not destroy the completion and the connection would
    // leak (completion -> connection -> lambda -> state -> handle -> frame ->
    // completion cycle).
    auto task = captureAwaitRaw(c.get(), &cap);

    // The coroutine has started eagerly and suspended at co_await *c.
    QVERIFY(!cap.done);

    // Drop the last external reference. ~DragonCompletion emits finished() with
    // a cancelled result, which resumes the suspended coroutine safely (no
    // use-after-free, no leaked frame). This replaces the old behavior of
    // m_handle.destroy()-ing the frame, which fought QCoro's ref-counting.
    c.reset();

    // Pump the event loop so the direct-connection resume completes.
    QTest::qWait(10);

    QVERIFY(cap.done);
    QVERIFY(!cap.result.success);
    QVERIFY(cap.result.cancelled);
}

void TestCompletion::testCancelUnderLockDeadlock()
{
    auto c = std::make_shared<DragonCompletion>();
    std::timed_mutex mtx;
    std::atomic slotExecuted{false};
    std::atomic slotDeadlocked{false};

    // Connect a slot that tries to acquire the same mutex
    connect(c.get(), &DragonCompletion::finished, c.get(), [&mtx, &slotExecuted, &slotDeadlocked](const InitResult &) {
        // Try to lock the mutex with a timeout to detect deadlock
        std::unique_lock lock(mtx, std::chrono::milliseconds(100));
        if (lock.owns_lock()) {
            slotExecuted.store(true);
        } else {
            slotDeadlocked.store(true);
        }
    });

    {
        std::scoped_lock lock(mtx);
        c->cancel(QStringLiteral("Test cancellation under lock"));
    }

    QTest::qWait(50);

    // This test demonstrates the deadlock pattern that exists in DragonCompletion
    // when cancel()/setResult() are called under a lock. The fix is at the call-site
    // (extract under lock, call outside), not in DragonCompletion itself.
    QEXPECT_FAIL("", "Demonstrates deadlock pattern - fixed at call-site in dragondecodepipeline.cpp", Continue);
    QVERIFY2(slotExecuted.load() && !slotDeadlocked.load(),
             "Expected: slot executes without deadlock. "
             "Bug: cancel() emits synchronously under lock, causing deadlock.");
}

QTEST_MAIN(TestCompletion)
#include "test_completion.moc"
