/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "sink/streamclock.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace
{

// Records the two audio-thread events so boundary behavior is directly
// observable. The clock runs on the test thread here; the handlers only
// record.
struct RecordedEvents {
    std::vector<std::pair<std::uint32_t, std::optional<std::chrono::milliseconds>>> trackStarted;
    std::vector<std::pair<int, int>> formatChangeRequired;

    void installOn(StreamClock &clock)
    {
        StreamClock::EventHandlers handlers;
        handlers.trackStarted = [this](std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration, std::uint64_t) {
            trackStarted.emplace_back(trackId, duration);
        };
        handlers.formatChangeRequired = [this](int sampleRate, int channels, std::uint64_t) {
            formatChangeRequired.emplace_back(sampleRate, channels);
        };
        clock.setEventHandlers(std::move(handlers));
    }
};

// The staging idiom every switch uses: mint the epoch first, then gate.
struct StagedSwitch {
    std::uint64_t epoch = 0;
};

StagedSwitch stageSwitch(StreamClock &clock)
{
    StagedSwitch staged;
    staged.epoch = clock.reserveEpoch();
    clock.armSwitch();
    return staged;
}

}

class TestStreamClock : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFreshClockState();
    void testProducerCounterFollowsEpoch();
    void testBoundaryFiresAfterReadAndRewindsWrittenCounter();
    void testStaleEpochBoundaryDiscarded();
    void testFormatStallClampsBudgetAndDedups();
    void testEosClaimLifecycle();
    void testEosRetract();
    void testBeginStreamPublishesNewStream();
    void testSwitchStagingAndConsumption();
    void testPositionTrackingReset();
    void testUnderrunCounter();
    void testBoundaryRingUseAfterPopRace();
    void testStaleEpochSamplesProducedRejected();
    void testProducerGateTransitions();
    void testEosEpochTagInterleavings();
    void testEosArmedChecksAreEpochScoped();
    void testClockResetUnsticksStaleEos();
    void testTwoBoundariesInOneQuantum();
    void testEosConcurrentBoundaryVsEpochSwitchRace();
    void testReserveEpochInvalidatesClaimedDrainedEmission();
};

void TestStreamClock::testFreshClockState()
{
    StreamClock clock;
    QVERIFY(clock.isQueueReady());
    QVERIFY(!clock.isSwitchArmed());
    QCOMPARE(clock.totalSamplesWritten(), 0);
    QCOMPARE(clock.streamSamplesRead(), 0);
    QCOMPARE(clock.producerSamples(), 0);
    QCOMPARE(clock.streamEpoch(), std::uint64_t{1});
    QCOMPARE(clock.underrunCount(), 0);
    clock.setFormat(44100, 2);
    QVERIFY(clock.hasFormat(44100, 2));
    QVERIFY(!clock.hasFormat(48000, 2));
}

void TestStreamClock::testProducerCounterFollowsEpoch()
{
    StreamClock clock;

    // Stale-epoch production is discarded; only the live epoch counts.
    clock.samplesProduced(clock.streamEpoch(), 100);
    QCOMPARE(clock.producerSamples(), 100);

    const auto staged = stageSwitch(clock);
    clock.beginStream(staged.epoch);
    QCOMPARE(clock.producerSamples(), 0);
    clock.samplesProduced(1, 50); // superseded epoch
    QCOMPARE(clock.producerSamples(), 0);
    clock.samplesProduced(clock.streamEpoch(), 30);
    QCOMPARE(clock.producerSamples(), 30);
}

void TestStreamClock::testBoundaryFiresAfterReadAndRewindsWrittenCounter()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    RecordedEvents events;
    events.installOn(clock);

    clock.samplesProduced(clock.streamEpoch(), 100);
    QVERIFY(clock.boundaryTrackStart(7, 44100, 2, std::chrono::milliseconds{5000}, clock.streamEpoch()));

    // Not read up to the boundary yet: nothing fires, nothing is counted.
    clock.noteSamplesRead(60);
    clock.noteSamplesRendered(60);
    QCOMPARE(clock.totalSamplesWritten(), 60);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::NoneCrossed);
    QVERIFY(events.trackStarted.empty());

    // Cross the boundary: trackStarted fires with id and duration, and the
    // written counter rewinds so position restarts at the new track.
    clock.noteSamplesRead(40);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::Crossed);
    QCOMPARE(events.trackStarted.size(), size_t{1});
    QCOMPARE(events.trackStarted[0].first, std::uint32_t{7});
    QCOMPARE(events.trackStarted[0].second, std::optional<std::chrono::milliseconds>{5000ms});
    QCOMPARE(clock.totalSamplesWritten(), 0); // 100 read - 100 boundary sample

    // A second boundary mid-track rewinds to the samples read past it.
    // (The producer counter accumulates across the epoch: boundary 8 sits at
    // producer sample 150.)
    clock.samplesProduced(clock.streamEpoch(), 50);
    QVERIFY(clock.boundaryTrackStart(8, 44100, 2, std::nullopt, clock.streamEpoch()));
    clock.noteSamplesRead(60);
    clock.noteSamplesRendered(60);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::Crossed);
    QCOMPARE(events.trackStarted.size(), size_t{2});
    QCOMPARE(events.trackStarted[1].first, std::uint32_t{8});
    QCOMPARE(clock.totalSamplesWritten(), 10); // 160 read - 150 boundary sample
}

void TestStreamClock::testStaleEpochBoundaryDiscarded()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    RecordedEvents events;
    events.installOn(clock);

    // Boundary staged for the current epoch, then a flush supersedes the
    // stream: the boundary must never fire as a ghost track start.
    clock.samplesProduced(clock.streamEpoch(), 100);
    QVERIFY(clock.boundaryTrackStart(7, 44100, 2, std::nullopt, clock.streamEpoch()));
    const auto staged = stageSwitch(clock);
    clock.beginStream(staged.epoch);

    clock.noteSamplesRead(100);
    clock.advanceBoundaries(true, nullptr);
    QVERIFY(events.trackStarted.empty());
}

void TestStreamClock::testFormatStallClampsBudgetAndDedups()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    RecordedEvents events;
    events.installOn(clock);

    // A format-changing boundary 100 samples out clamps the pre-read budget.
    clock.samplesProduced(clock.streamEpoch(), 100);
    QVERIFY(clock.boundaryTrackStart(9, 48000, 2, std::nullopt, clock.streamEpoch()));

    std::size_t budget = 1000;
    clock.advanceBoundaries(false, &budget);
    QCOMPARE(budget, std::size_t{100});

    // Reaching the boundary stalls rendering and asks for the new format,
    // exactly once while stalled.
    clock.noteSamplesRead(100);
    QCOMPARE(clock.advanceBoundaries(false, nullptr), StreamClock::BoundaryAdvance::FormatStall);
    QCOMPARE(events.formatChangeRequired.size(), size_t{1});
    QCOMPARE(events.formatChangeRequired[0].first, 48000);
    QCOMPARE(events.formatChangeRequired[0].second, 2);
    QCOMPARE(clock.advanceBoundaries(false, nullptr), StreamClock::BoundaryAdvance::FormatStall);
    QCOMPARE(events.formatChangeRequired.size(), size_t{1});

    // Once the format matches (the caller reopened at 48 kHz) the boundary
    // passes and fires its track start.
    clock.setFormat(48000, 2);
    clock.noteSamplesRead(1);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::Crossed);
    QCOMPARE(events.trackStarted.size(), size_t{1});
    QCOMPARE(events.trackStarted[0].first, std::uint32_t{9});
}

void TestStreamClock::testEosClaimLifecycle()
{
    StreamClock clock;

    // EOS arms at the current producer position.
    clock.samplesProduced(clock.streamEpoch(), 500);
    QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
    QVERIFY(clock.endOfStreamArmed());
    QVERIFY(!clock.endOfStreamPending()); // nothing read yet

    // Claiming before the stream is drained fails.
    QVERIFY(!clock.tryClaimDrained());

    // Once fully read the drain can be claimed exactly once, and the
    // emission can be taken exactly once.
    clock.noteSamplesRead(500);
    QVERIFY(clock.endOfStreamPending());
    QVERIFY(clock.tryClaimDrained());
    QVERIFY(!clock.endOfStreamPending()); // claimed, not pending anymore
    QVERIFY(!clock.tryClaimDrained()); // cannot claim twice
    QVERIFY(clock.takeDrainedEmission());
    QVERIFY(!clock.takeDrainedEmission());

    // Emitted is terminal within a stream: re-arming requires a fresh
    // stream (what the crossfade latch / flush publishes).
    QVERIFY(!clock.boundaryEndOfStream(clock.streamEpoch()));
    const auto staged = stageSwitch(clock);
    clock.beginStream(staged.epoch);
    clock.samplesProduced(clock.streamEpoch(), 10);
    QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
}

void TestStreamClock::testEosRetract()
{
    StreamClock clock;
    clock.samplesProduced(clock.streamEpoch(), 10);
    QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
    QVERIFY(clock.retractEndOfStream());
    QVERIFY(!clock.retractEndOfStream()); // nothing pending anymore
    QVERIFY(!clock.endOfStreamArmed());
    QVERIFY(!clock.endOfStreamPending());
}

void TestStreamClock::testBeginStreamPublishesNewStream()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    RecordedEvents events;
    events.installOn(clock);

    clock.samplesProduced(clock.streamEpoch(), 200);
    clock.boundaryTrackStart(1, 44100, 2, std::nullopt, clock.streamEpoch());
    clock.boundaryEndOfStream(clock.streamEpoch());
    clock.noteSamplesRead(200);
    clock.noteSamplesRendered(200);
    const std::uint64_t oldEpoch = clock.streamEpoch();
    const auto staged = stageSwitch(clock);
    QVERIFY(!clock.isQueueReady());

    clock.beginStream(staged.epoch);

    QVERIFY(clock.streamEpoch() != oldEpoch);
    QCOMPARE(clock.producerSamples(), 0);
    QCOMPARE(clock.streamSamplesRead(), 0);
    QCOMPARE(clock.totalSamplesWritten(), 0);
    QVERIFY(clock.isQueueReady());
    QVERIFY(!clock.endOfStreamArmed());
    QVERIFY(!clock.endOfStreamPending());

    // Staged boundaries were drained: no ghost track start for the old stream.
    clock.advanceBoundaries(true, nullptr);
    QVERIFY(events.trackStarted.empty());
}

void TestStreamClock::testSwitchStagingAndConsumption()
{
    StreamClock clock;

    // Staging a switch gates the producers and is one flag, not three:
    // armSwitch() + reserveEpoch() publish exactly one epoch.
    const auto staged = stageSwitch(clock);
    QVERIFY(!clock.isQueueReady());
    QVERIFY(clock.isSwitchArmed());
    QVERIFY(clock.streamEpoch() == staged.epoch);
    QVERIFY(clock.producerSamples() == 0);

    // The audio thread publishes under the staged epoch and reopens the gate.
    clock.beginStream(staged.epoch);
    QVERIFY(clock.isQueueReady());
    QVERIFY(!clock.isSwitchArmed());

    // The staged epoch is returned to the caller, never predicted: publishing
    // under it does not bump the epoch again.
    QCOMPARE(clock.streamEpoch(), staged.epoch);
}

void TestStreamClock::testPositionTrackingReset()
{
    StreamClock clock;
    clock.setPositionOffset(900ms);
    clock.noteSamplesRendered(1234);
    clock.resetPositionTracking();
    QCOMPARE(clock.totalSamplesWritten(), 0);
    QCOMPARE(clock.positionOffset(), 0ms);

    // resetWrittenCounter (the primary Voice stream reopen) clears only the counter.
    clock.setPositionOffset(500ms);
    clock.noteSamplesRendered(99);
    clock.resetWrittenCounter();
    QCOMPARE(clock.totalSamplesWritten(), 0);
    QCOMPARE(clock.positionOffset(), 500ms);
}

void TestStreamClock::testUnderrunCounter()
{
    StreamClock clock;
    QCOMPARE(clock.underrunCount(), 0);
    clock.noteUnderrun();
    clock.noteUnderrun();
    QCOMPARE(clock.underrunCount(), 2);
}

void TestStreamClock::testBoundaryRingUseAfterPopRace()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    std::atomic<bool> stop{false};
    std::atomic<int> corruptionCount{0};

    StreamClock::EventHandlers handlers;
    handlers.trackStarted = [&](std::uint32_t trackId, std::optional<std::chrono::milliseconds> duration, std::uint64_t) {
        if (!duration || duration->count() != static_cast<qint64>(trackId) * 1000) {
            corruptionCount.fetch_add(1, std::memory_order_relaxed);
        }
    };
    clock.setEventHandlers(std::move(handlers));

    const auto epoch = clock.streamEpoch();

    std::jthread producer([&] {
        std::uint32_t id = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            clock.samplesProduced(epoch, 10);
            if (clock.boundaryTrackStart(id, 44100, 2, std::chrono::milliseconds{static_cast<qint64>(id) * 1000}, epoch)) {
                ++id;
            }
        }
    });

    for (int i = 0; i < 50000; ++i) {
        clock.noteSamplesRead(100);
        clock.advanceBoundaries(true, nullptr);
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();

    QVERIFY2(corruptionCount.load() == 0,
             qPrintable(QStringLiteral("Detected %1 boundary corruptions due to use-after-pop in advanceBoundaries()").arg(corruptionCount.load())));
}

// Invariant test: samplesProduced() with a stale epoch tag is safely rejected by
// the clock and does not corrupt the current track's EOS sample count.
// Note: the player-level demotion guard routing demoted writers away from
// samplesProduced() is tested in TestPlayerPlayback::testDemotedCrossfadeWriterDoesNotCorruptProducerState.
void TestStreamClock::testStaleEpochSamplesProducedRejected()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    // Track 1 begins: epoch 1
    const auto track1Epoch = clock.streamEpoch();
    QCOMPARE(track1Epoch, std::uint64_t{1});

    // Crossfade occurs to Track 2: the switch stages epoch 2, the latch publishes it
    const auto track2Staged = stageSwitch(clock);
    QCOMPARE(track2Staged.epoch, std::uint64_t{2});
    clock.beginStream(track2Staged.epoch);
    const auto track2Epoch = clock.streamEpoch();
    QCOMPARE(track2Epoch, std::uint64_t{2});

    // Track 2 decodes 1000 samples
    clock.samplesProduced(track2Epoch, 1000);

    // Demoted Track 1's decode thread wakes up. Because of session tokens and epoch tagging,
    // it produces samples with track1Epoch (1), which is safely rejected by the clock and
    // does not corrupt Track 2's EOS sample count.
    clock.samplesProduced(track1Epoch, 200);

    // Track 2 finishes decoding and announces EOS
    clock.boundaryEndOfStream(track2Epoch);

    // Track 2 plays out all of its 1000 samples from the primary pipe
    clock.noteSamplesRead(1000);

    // Primary track has been fully consumed. EOS must now be claimable!
    QVERIFY2(clock.endOfStreamPending(), "endOfStreamPending() is false after consuming all primary samples because fading producer corrupted m_eosSample!");
    QVERIFY(clock.tryClaimDrained());
}

void TestStreamClock::testProducerGateTransitions()
{
    StreamClock clock;

    // Open -> SwitchArmed -> Open via beginStream
    QVERIFY(clock.isQueueReady());
    const auto staged = stageSwitch(clock);
    QVERIFY(clock.isSwitchArmed());
    QVERIFY(!clock.isQueueReady());
    clock.beginStream(staged.epoch);
    QVERIFY(clock.isQueueReady());
    QVERIFY(!clock.isSwitchArmed());

    // reopenIfIdle cannot touch SwitchArmed
    const auto armed = stageSwitch(clock);
    QVERIFY(!clock.reopenIfIdle());
    QVERIFY(!clock.isQueueReady());

    // beginStream reopens from SwitchArmed even after reopenIfIdle refused
    clock.beginStream(armed.epoch);
    QVERIFY(clock.isQueueReady());

    // reopenIfIdle transitions Closed -> Open
    clock.closeGate();
    QVERIFY(!clock.isQueueReady());
    QVERIFY(clock.reopenIfIdle());
    QVERIFY(clock.isQueueReady());

    // armSwitch takes over a Closed gate (the seek path stages after closeGate)
    clock.closeGate();
    const auto takeover = stageSwitch(clock);
    QVERIFY(clock.isSwitchArmed());
    clock.beginStream(takeover.epoch);
    QVERIFY(clock.isQueueReady());

    // Render consumes the switch and begins the stream before main thread reopens
    const auto staged2 = stageSwitch(clock);
    clock.beginStream(staged2.epoch);
    QVERIFY(clock.isQueueReady());
    // Main thread subsequently executes reopenIfIdle() on resume
    QVERIFY(!clock.reopenIfIdle()); // Already open, stays open
    QVERIFY(clock.isQueueReady()); // Never deadlocks!
}

// The EOS must survive every interleaving of its arming with a stream switch:
// the epoch tag recorded before and re-verified after the None->Pending CAS
// catches a switch that published in between.
void TestStreamClock::testEosEpochTagInterleavings()
{
    // Interleaving 1: EOS armed, then the switch publishes. The EOS belongs
    // to the old stream; beginStream resets the EOS state with it.
    {
        StreamClock clock;
        clock.samplesProduced(clock.streamEpoch(), 500);
        QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
        const auto staged = stageSwitch(clock);
        clock.beginStream(staged.epoch);
        QVERIFY(!clock.endOfStreamArmed());
    }

    // Interleaving 2: the switch publishes (new epoch) while the EOS arming
    // is mid-flight; the tag check rejects the stale EOS instead of applying
    // it to the new stream with a bogus sample count.
    {
        StreamClock clock;
        const auto oldEpoch = clock.streamEpoch();
        clock.samplesProduced(oldEpoch, 500);
        const auto staged = stageSwitch(clock);
        clock.beginStream(staged.epoch);
        // Producer's EOS arming arrives after the new stream published.
        QVERIFY(!clock.boundaryEndOfStream(oldEpoch));
        QVERIFY(!clock.endOfStreamArmed());
        // EOS for the NEW stream arms cleanly.
        clock.samplesProduced(clock.streamEpoch(), 20);
        QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
        QVERIFY(clock.endOfStreamArmed());
    }

    // Interleaving 3: EOS CAS wins the race against beginStream's EOS reset.
    // The CAS records Pending before beginStream resets to None; the
    // post-CAS epoch re-verification retracts it.
    {
        StreamClock clock;
        const auto oldEpoch = clock.streamEpoch();
        clock.samplesProduced(oldEpoch, 500);
        const auto staged = stageSwitch(clock);
        // Simulate: CAS succeeded, then beginStream ran, then the re-check.
        // Drive it through the real API: arm under oldEpoch is refused after
        // the publish because the epoch no longer matches.
        clock.beginStream(staged.epoch);
        QVERIFY(!clock.boundaryEndOfStream(oldEpoch));
        QVERIFY(!clock.endOfStreamArmed());
    }
}

void TestStreamClock::testEosArmedChecksAreEpochScoped()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    const auto oldEpoch = clock.streamEpoch();
    clock.samplesProduced(oldEpoch, 100);
    QVERIFY(clock.boundaryEndOfStream(oldEpoch));
    clock.noteSamplesRead(100);
    QVERIFY(clock.tryClaimDrained());
    QVERIFY(clock.endOfStreamArmed());

    const auto newEpoch = clock.reserveEpoch();
    QVERIFY(newEpoch != oldEpoch);
    QVERIFY(!clock.endOfStreamArmed());
    QVERIFY(!clock.endOfStreamPending());
    QVERIFY(!clock.tryClaimDrained());
    QVERIFY(!clock.takeDrainedEmission());
    QVERIFY(!clock.retractEndOfStream());

    clock.beginStream(newEpoch);
    QVERIFY(!clock.endOfStreamArmed());
    clock.samplesProduced(newEpoch, 20);
    QVERIFY(clock.boundaryEndOfStream(newEpoch));
    QVERIFY(clock.endOfStreamArmed());
    QVERIFY(clock.retractEndOfStream());
    QVERIFY(!clock.endOfStreamArmed());
}

// A stale Claimed/Emitted EOS (left over from a torn-down stream) must not
// block the next stream's drained() handling: the full clock reset clears it.
void TestStreamClock::testClockResetUnsticksStaleEos()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    clock.samplesProduced(clock.streamEpoch(), 100);
    QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
    clock.noteSamplesRead(100);
    QVERIFY(clock.tryClaimDrained());
    QVERIFY(clock.takeDrainedEmission());
    QVERIFY(clock.endOfStreamArmed()); // stale Emitted state

    clock.reset();
    QVERIFY(!clock.endOfStreamArmed());
    QVERIFY(clock.isQueueReady());
    QVERIFY(!clock.isSwitchArmed());
    QCOMPARE(clock.positionOffset(), 0ms);

    // The next stream can arm EOS again (this is the fix for the stale-EOS
    // blocking drained() after stop/reset).
    const auto staged = stageSwitch(clock);
    clock.beginStream(staged.epoch);
    clock.samplesProduced(clock.streamEpoch(), 50);
    QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
    clock.noteSamplesRead(50);
    QVERIFY(clock.tryClaimDrained());
}

// Two boundaries within one read quantum: a same-format boundary is crossed
// mid-quantum (no budget clamp, so gapless playback never sees a short
// render), and a different-format boundary behind it clamps the budget so
// no wrong-format samples are pulled past it.
void TestStreamClock::testTwoBoundariesInOneQuantum()
{
    StreamClock clock;
    clock.setFormat(44100, 2);
    RecordedEvents events;
    events.installOn(clock);

    const auto epoch = clock.streamEpoch();
    // Same-format boundary at 100, different-format boundary at 200.
    clock.samplesProduced(epoch, 100);
    QVERIFY(clock.boundaryTrackStart(11, 44100, 2, std::nullopt, epoch));
    clock.samplesProduced(epoch, 100);
    QVERIFY(clock.boundaryTrackStart(12, 48000, 2, std::nullopt, epoch));

    // The first boundary is same-format: the budget is NOT clamped to it.
    clock.noteSamplesRead(50);
    std::size_t budget = 1000;
    QCOMPARE(clock.advanceBoundaries(false, &budget), StreamClock::BoundaryAdvance::NoneCrossed);
    QCOMPARE(budget, std::size_t{150}); // clamped at the format-changing boundary (200 - 50), not at 100

    // Reading exactly the clamped budget crosses the first boundary
    // mid-quantum and fires its track start.
    clock.noteSamplesRead(150);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::Crossed);
    QCOMPARE(events.trackStarted.size(), size_t{1});
    QCOMPARE(events.trackStarted[0].first, std::uint32_t{11});

    // The second boundary stalls and demands the new format.
    clock.noteSamplesRead(50);
    QCOMPARE(clock.advanceBoundaries(false, nullptr), StreamClock::BoundaryAdvance::FormatStall);
    QCOMPARE(events.formatChangeRequired.size(), size_t{1});
    clock.setFormat(48000, 2);
    clock.noteSamplesRead(1);
    QCOMPARE(clock.advanceBoundaries(true, nullptr), StreamClock::BoundaryAdvance::Crossed);
    QCOMPARE(events.trackStarted.size(), size_t{2});
    QCOMPARE(events.trackStarted[1].first, std::uint32_t{12});
}

void TestStreamClock::testEosConcurrentBoundaryVsEpochSwitchRace()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    std::atomic<std::uint64_t> activeEpoch{clock.streamEpoch()};
    std::atomic<bool> stop{false};
    std::atomic<int> falseClaims{0};
    std::atomic<int> producerAttempts{0};

    std::jthread producer([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint64_t epoch = activeEpoch.load(std::memory_order_acquire);
            clock.samplesProduced(epoch, 64);
            std::ignore = clock.boundaryEndOfStream(epoch);
            producerAttempts.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 4000; ++i) {
        const auto nextEpoch = clock.reserveEpoch();
        clock.armSwitch();
        clock.beginStream(nextEpoch);

        // Producer still only knows previous epochs until activeEpoch is published
        // below. Any EOS leaked from a prior epoch onto nextEpoch would trigger here.
        clock.noteSamplesRead(64);
        if (clock.endOfStreamPending() || clock.tryClaimDrained()) {
            falseClaims.fetch_add(1, std::memory_order_relaxed);
        }

        activeEpoch.store(nextEpoch, std::memory_order_release);
        if ((i % 32) == 0) {
            std::this_thread::yield();
        }
    }

    stop.store(true, std::memory_order_release);
    producer.join();

    QCOMPARE(falseClaims.load(std::memory_order_relaxed), 0);
    QVERIFY(producerAttempts.load(std::memory_order_relaxed) > 0);
}

void TestStreamClock::testReserveEpochInvalidatesClaimedDrainedEmission()
{
    StreamClock clock;
    clock.setFormat(44100, 2);

    const auto epoch1 = clock.streamEpoch();
    clock.samplesProduced(epoch1, 100);
    QVERIFY(clock.boundaryEndOfStream(epoch1));
    clock.noteSamplesRead(100);
    QVERIFY(clock.tryClaimDrained());

    // Simulate a seek or switch staging on the main thread before the queued
    // drained notification executes takeDrainedEmission().
    const auto epoch2 = clock.reserveEpoch();
    QVERIFY(!clock.takeDrainedEmission());

    // Contrast: on the fresh epoch, claiming without a subsequent reserveEpoch()
    // succeeds once and only once.
    clock.armSwitch();
    clock.beginStream(epoch2);
    clock.samplesProduced(epoch2, 100);
    QVERIFY(clock.boundaryEndOfStream(epoch2));
    clock.noteSamplesRead(100);
    QVERIFY(clock.tryClaimDrained());
    QVERIFY(clock.takeDrainedEmission());
    QVERIFY(!clock.takeDrainedEmission());
}

QTEST_GUILESS_MAIN(TestStreamClock)
#include "test_streamclock.moc"
