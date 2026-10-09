/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "decoder/dragondecodepipeline.h"
#include <DragonPlayer>

#include <QCoroTask>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;

namespace
{

struct InitCapture {
    DragonMediaBackend::InitResult result;
    std::atomic<bool> done{false};
};

QCoro::Task<void> runInitialize(DragonDecodePipeline &pipeline, const QUrl &url, InitCapture *cap)
{
    cap->result = co_await pipeline.initializeSession(url);
    cap->done.store(true, std::memory_order_release);
}

struct PromotionWindow {
    std::atomic<bool> entered{false};
    std::atomic<bool> callbackDone{false};

    void install(DragonDecodePipeline &pipeline)
    {
        pipeline.setBoundaryCallback([this](const DragonDecodePipeline::BoundaryEvent &event) -> bool {
            if (event.kind == DragonDecodePipeline::BoundaryEvent::Kind::TrackStart && !entered.exchange(true, std::memory_order_acq_rel)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                callbackDone.store(true, std::memory_order_release);
            }
            return true;
        });
    }
};

class FinishedLog
{
public:
    void append(const QUrl &source)
    {
        std::scoped_lock lock(m_mutex);
        m_sources.push_back(source);
    }

    [[nodiscard]] bool contains(const QUrl &source) const
    {
        std::scoped_lock lock(m_mutex);
        return std::ranges::any_of(m_sources, [&source](const QUrl &s) {
            return s == source;
        });
    }

    [[nodiscard]] std::size_t size() const
    {
        std::scoped_lock lock(m_mutex);
        return m_sources.size();
    }

private:
    mutable std::mutex m_mutex;
    std::vector<QUrl> m_sources;
};

} // namespace

class TestDecodePipelinePromotion : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSetNextSourceDuringInFlightPromotionClearsSlot();
    void testSetNextSourceDuringInFlightPromotionReplacesSlot();
    void testStopSessionWaitsForInFlightPromotion();
    void testRequestSeekWaitsForInFlightPromotion();

    // BoundaryEvent must report seekable=true for a promoted seekable track.
    void testBoundaryEventCarriesSeekable();

    // Multiple sequential promotions must keep the current source updated
    // so the final track reports its own URL on sessionFinished.
    void testSequentialPromotionsMaintainSourceIntegrity();

private:
    void runPromotionRace(const QUrl &raceReplacement);
};

void TestDecodePipelinePromotion::runPromotionRace(const QUrl &raceReplacement)
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.ogg"_s));
    if (!raceReplacement.isEmpty()) {
        QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.m4a"_s));
    }

    const QUrl url1 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl url2 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s));
    const QUrl url3 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s));

    DragonPlayer player; // parent and signal target for the pipeline under test
    DragonDecodePipeline pipeline(&player);

    std::atomic<bool> decodeGate{false};
    pipeline.setSamplesCallback([&decodeGate](std::span<const float>, const std::stop_token &st) {
        while (!decodeGate.load(std::memory_order_acquire) && !st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    InitCapture init;
    auto initTask = runInitialize(pipeline, url1, &init);
    QVERIFY(QTest::qWaitFor(
        [&init]() {
            return init.done.load(std::memory_order_acquire);
        },
        10000));
    QVERIFY2(init.result.success, qPrintable(init.result.errorMessage));

    // Fill the next-track slot while track 1's decode is parked in the gate.
    pipeline.setNextSource(url2);
    QTest::qWait(1000); // local 3s fixtures pre-warm in milliseconds

    FinishedLog finished;
    std::atomic<bool> trackStartSeen{false};
    std::atomic<bool> raceDone{false};
    QObject::connect(&pipeline, &DragonDecodePipeline::sessionFinished, [&finished](const QUrl &source, bool hadFatalError) {
        Q_UNUSED(hadFatalError);
        finished.append(source);
    });

    pipeline.setBoundaryCallback([&pipeline, &trackStartSeen, &raceDone, raceReplacement](const DragonDecodePipeline::BoundaryEvent &event) -> bool {
        if (event.kind == DragonDecodePipeline::BoundaryEvent::Kind::TrackStart) {
            trackStartSeen.store(true, std::memory_order_release);
            if (!raceDone.exchange(true, std::memory_order_acq_rel)) {
                pipeline.setNextSource(raceReplacement);
            }
        }
        return true;
    });

    decodeGate.store(true, std::memory_order_release);

    QTRY_VERIFY_WITH_TIMEOUT(trackStartSeen.load(std::memory_order_acquire), 15000);

    const QUrl &expectedFinalSource = raceReplacement.isEmpty() ? url2 : url3;
    QTRY_VERIFY_WITH_TIMEOUT(finished.contains(expectedFinalSource), 15000);
    QVERIFY2(!finished.contains(url1), "the raced promotion must not be dropped: no sessionFinished may be reported for the first track");
    QCOMPARE(finished.size(), std::size_t{1});
}

void TestDecodePipelinePromotion::testSetNextSourceDuringInFlightPromotionClearsSlot()
{
    runPromotionRace(QUrl());
}

void TestDecodePipelinePromotion::testSetNextSourceDuringInFlightPromotionReplacesSlot()
{
    runPromotionRace(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s)));
}

void TestDecodePipelinePromotion::testStopSessionWaitsForInFlightPromotion()
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.ogg"_s));

    const QUrl url1 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl url2 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s));

    DragonPlayer player; // parent and signal target for the pipeline under test
    DragonDecodePipeline pipeline(&player);

    std::atomic<bool> decodeGate{false};
    pipeline.setSamplesCallback([&decodeGate](std::span<const float>, const std::stop_token &st) {
        while (!decodeGate.load(std::memory_order_acquire) && !st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    InitCapture init;
    auto initTask = runInitialize(pipeline, url1, &init);
    QVERIFY(QTest::qWaitFor(
        [&init]() {
            return init.done.load(std::memory_order_acquire);
        },
        10000));
    QVERIFY2(init.result.success, qPrintable(init.result.errorMessage));

    pipeline.setNextSource(url2);
    QTest::qWait(1000); // local 3s fixtures pre-warm in milliseconds

    PromotionWindow window;
    window.install(pipeline);

    decodeGate.store(true, std::memory_order_release);
    QVERIFY(QTest::qWaitFor(
        [&window]() {
            return window.entered.load(std::memory_order_acquire);
        },
        15000));

    pipeline.stopSession();

    QVERIFY2(window.callbackDone.load(std::memory_order_acquire), "stopSession() must not return while a promotion is recording its boundary");
}

void TestDecodePipelinePromotion::testRequestSeekWaitsForInFlightPromotion()
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.ogg"_s));

    const QUrl url1 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl url2 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s));

    DragonPlayer player; // parent and signal target for the pipeline under test
    DragonDecodePipeline pipeline(&player);

    std::atomic<bool> decodeGate{false};
    pipeline.setSamplesCallback([&decodeGate](std::span<const float>, const std::stop_token &st) {
        while (!decodeGate.load(std::memory_order_acquire) && !st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    InitCapture init;
    auto initTask = runInitialize(pipeline, url1, &init);
    QVERIFY(QTest::qWaitFor(
        [&init]() {
            return init.done.load(std::memory_order_acquire);
        },
        10000));
    QVERIFY2(init.result.success, qPrintable(init.result.errorMessage));

    pipeline.setNextSource(url2);
    QTest::qWait(1000); // local 3s fixtures pre-warm in milliseconds

    FinishedLog finished;
    QObject::connect(&pipeline, &DragonDecodePipeline::sessionFinished, [&finished](const QUrl &source, bool hadFatalError) {
        Q_UNUSED(hadFatalError);
        finished.append(source);
    });

    PromotionWindow window;
    window.install(pipeline);

    decodeGate.store(true, std::memory_order_release);
    QVERIFY(QTest::qWaitFor(
        [&window]() {
            return window.entered.load(std::memory_order_acquire);
        },
        15000));

    pipeline.requestSeek(std::chrono::milliseconds{500});

    QVERIFY2(window.callbackDone.load(std::memory_order_acquire), "requestSeek() must not return while a promotion is recording its boundary");

    // The promotion completed and the sought track decodes to completion.
    QTRY_VERIFY_WITH_TIMEOUT(finished.contains(url2), 15000);
}

void TestDecodePipelinePromotion::testBoundaryEventCarriesSeekable()
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.ogg"_s));

    const QUrl url1 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl url2 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s));

    DragonPlayer player;
    DragonDecodePipeline pipeline(&player);

    std::atomic<bool> seekableSeen{false};
    std::atomic<bool> boundarySeen{false};
    pipeline.setBoundaryCallback([&](const DragonDecodePipeline::BoundaryEvent &event) -> bool {
        if (event.kind == DragonDecodePipeline::BoundaryEvent::Kind::TrackStart) {
            seekableSeen.store(event.seekable, std::memory_order_release);
            boundarySeen.store(true, std::memory_order_release);
        }
        return true;
    });

    InitCapture init;
    auto initTask = runInitialize(pipeline, url1, &init);
    QVERIFY(QTest::qWaitFor(
        [&init]() {
            return init.done.load(std::memory_order_acquire);
        },
        10000));
    QVERIFY2(init.result.success, qPrintable(init.result.errorMessage));

    pipeline.setNextSource(url2);

    QVERIFY(QTest::qWaitFor(
        [&boundarySeen]() {
            return boundarySeen.load(std::memory_order_acquire);
        },
        15000));
    QVERIFY2(seekableSeen.load(std::memory_order_acquire), "Promoted local file track must be marked seekable in BoundaryEvent");
}

void TestDecodePipelinePromotion::testSequentialPromotionsMaintainSourceIntegrity()
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.ogg"_s));
    QVERIFY(TestFixture::verifyExists(u"gs-3s-2c-44100hz.m4a"_s));

    const QUrl url1 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));
    const QUrl url2 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s));
    const QUrl url3 = QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.m4a"_s));

    DragonPlayer player;
    DragonDecodePipeline pipeline(&player);

    FinishedLog finished;
    QObject::connect(&pipeline, &DragonDecodePipeline::sessionFinished, [&finished](const QUrl &source, bool hadFatalError) {
        Q_UNUSED(hadFatalError);
        finished.append(source);
    });

    std::atomic<int> promotions{0};
    pipeline.setBoundaryCallback([&pipeline, &promotions, url3](const DragonDecodePipeline::BoundaryEvent &event) -> bool {
        if (event.kind == DragonDecodePipeline::BoundaryEvent::Kind::TrackStart) {
            const int p = ++promotions;
            if (p == 1) {
                // When track 2 promotes, immediately arm track 3
                pipeline.setNextSource(url3);
            }
        }
        return true;
    });

    std::atomic<bool> decodeGate{false};
    pipeline.setSamplesCallback([&decodeGate](std::span<const float>, const std::stop_token &st) {
        while (!decodeGate.load(std::memory_order_acquire) && !st.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    InitCapture init;
    auto initTask = runInitialize(pipeline, url1, &init);
    QVERIFY(QTest::qWaitFor(
        [&init]() {
            return init.done.load(std::memory_order_acquire);
        },
        10000));
    QVERIFY2(init.result.success, qPrintable(init.result.errorMessage));

    // Fill the next-track slot while track 1's decode is parked in the gate.
    pipeline.setNextSource(url2);
    QTest::qWait(1000); // local 3s fixtures pre-warm in milliseconds

    // Open the gate: track 1 decodes to EOF and promotes track 2.
    // When track 2 promotes, the boundary callback arms track 3.
    // Track 2 decodes to EOF and promotes track 3.
    // Track 3 decodes to EOF and finalizes.
    decodeGate.store(true, std::memory_order_release);

    // Track 1 promotes track 2, then track 2 promotes track 3, and track 3 finishes.
    QTRY_VERIFY_WITH_TIMEOUT(finished.contains(url3), 15000);
    QVERIFY2(!finished.contains(url1), "No sessionFinished may be reported for track 1 (handed off gaplessly)");
    QVERIFY2(!finished.contains(url2), "No sessionFinished may be reported for track 2 (handed off gaplessly)");
    QCOMPARE(finished.size(), std::size_t{1});
}

QTEST_GUILESS_MAIN(TestDecodePipelinePromotion)
#include "test_decodepipeline_promotion.moc"
