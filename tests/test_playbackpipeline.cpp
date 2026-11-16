/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "fakesink.h"
#include "player/pipe.h"
#include "sink/playbackpipeline.h"

#include <QCoreApplication>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

using namespace std::chrono_literals;

namespace
{

constexpr float kLog100 = 4.60517018599f;

std::vector<float> makeConstant(float amplitude, int frames, int channels)
{
    std::vector<float> samples;
    samples.reserve(static_cast<size_t>(frames * channels));
    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            samples.push_back(amplitude);
        }
    }
    return samples;
}

constexpr int kTestFrames = 32000;
constexpr int kTestChannels = 2;

void writeAll(Pipe<float> &pipe, const std::vector<float> &samples)
{
    std::stop_source source;
    const size_t written = pipe.producer().write(samples, source.get_token());
    QCOMPARE(written, samples.size());
}

std::vector<float> drainSpan(std::span<const float> span)
{
    return {span.begin(), span.end()};
}

FakeAudioSink *fakeSinkOf(const PlaybackPipeline &pipeline)
{
    return static_cast<FakeAudioSink *>(pipeline.sink());
}

// The n-th stream that is still open on the fake backend.
FakeAudioSinkStream *liveStream(const PlaybackPipeline &pipeline, int index)
{
    return fakeSinkOf(pipeline)->liveStreams().at(static_cast<size_t>(index));
}

}

// Pipeline-level behavior on a backend-free fake sink: the pending primary
// voice, the two-stream crossfade window, stream-creation failure fallback,
// volume/mute/name fan-out, and fading-voice completion routing. Streams
// render when the test pumps them, standing in for backend audio callbacks.
class TestPlaybackPipeline : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFadeInFromClosedPreservedAcrossFormatReopen();
    void testArmCrossfadeCreatesPendingVoiceWithoutStream();
    void testIncomingStreamOpensAtNativeFormatAfterLoad();
    void testSecondStreamCreationFailureFallsBackToSolo();
    void testVolumeAndMuteFanOutToAllStreams();
    void testExternalVolumeOnlyFromPrimaryMirroredToFading();
    void testVolumeRestoreAdoptedOnlyForFirstStream();
    void testStreamNameFansOutAndAppliesToNewStreams();
    void testFadingCompletionNotifiesByPipeIdentity();
    void testHardSwitchDestroysBothVoices();
    void testResetReinstatesPendingPrimary();
};

void TestPlaybackPipeline::testFadeInFromClosedPreservedAcrossFormatReopen()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);

    // Stage a standalone fade-in while no stream exists (the device is
    // closed), matching DragonPlayer::setSource(url, fadeInOptions) from
    // StoppedState.
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 1000ms);
    QVERIFY(!pipeline.isFlushPending());

    // Open once the decoder resolves the format: setFormat() must preserve
    // the unstarted fade-in and scale it to the new rate.
    pipeline.open(44100, 2);
    QVERIFY(pipeline.isDeviceOpen());
    writeAll(pipe, makeConstant(0.5f, kTestFrames, kTestChannels));

    const auto out = drainSpan(liveStream(pipeline, 0)->pump(2048));
    QCOMPARE(out.size(), size_t{2048});
    QVERIFY2(qAbs(out[0]) < 1e-4f, "fade-in staged while closed must start at silence after open()");
    QVERIFY(out.back() > 1e-3f);

    // Conversely, staging a 0ms fade-in before open() clears any prior
    // unstarted fade-in.
    pipeline.close();
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 1000ms);
    std::ignore = pipeline.setPositionOffset(0ms, &pipe, 0ms);
    pipeline.open(44100, 2);
    writeAll(pipe, makeConstant(0.5f, 2048, kTestChannels));

    const auto unityOut = drainSpan(liveStream(pipeline, 0)->pump(2048));
    QCOMPARE(unityOut.size(), size_t{2048});
    QVERIFY(qAbs(unityOut[0] - 0.5f) < 1e-6f);
}

void TestPlaybackPipeline::testArmCrossfadeCreatesPendingVoiceWithoutStream()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), std::size_t{2048});

    // The incoming stream is opened only after its format is known: arming
    // creates a pending Voice, not a backend stream.
    const auto epoch = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(epoch > 0);
    QVERIFY(pipeline.isCrossfading());
    QCOMPARE(fakeSinkOf(pipeline)->createAttempts(), 1);

    // The outgoing voice keeps its stream; the new primary has none yet.
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{1});
    QVERIFY(!pipeline.isFlushPending());
}

void TestPlaybackPipeline::testIncomingStreamOpensAtNativeFormatAfterLoad()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);

    // The load resolves at a different native rate (96 kHz mono FLAC): the
    // incoming stream opens at that format while the outgoing keeps its own.
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, 1));
    pipeline.open(96000, 1);

    QCOMPARE(fakeSinkOf(pipeline)->createAttempts(), 2);
    QCOMPARE(liveStream(pipeline, 1)->sampleRate, 96000);
    QCOMPARE(liveStream(pipeline, 1)->channels, 1);
    QCOMPARE(liveStream(pipeline, 0)->sampleRate, 44100);
    QCOMPARE(liveStream(pipeline, 0)->channels, 2);

    // Both voices render concurrently: the server mixes them. The incoming
    // starts fading in from silence, releasing the outgoing's slow-load hold.
    const auto incoming = drainSpan(liveStream(pipeline, 1)->pump(2048));
    QCOMPARE(incoming.size(), size_t{2048});
    QVERIFY(qAbs(incoming[0]) < 1e-3f);

    const auto outgoing = drainSpan(liveStream(pipeline, 0)->pump(2048));
    QVERIFY(!outgoing.empty());
    QVERIFY(outgoing[outgoing.size() - 2] < outgoing[0]);
}

void TestPlaybackPipeline::testSecondStreamCreationFailureFallsBackToSolo()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);

    // Fail every stream creation after the first: open() keeps the failure
    // local while a fading voice exists and leaves the primary streamless.
    fakeSinkOf(pipeline)->failStreamsAfter(1);
    pipeline.open(44100, 2);
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(pipeline.isCrossfading());

    // The player aborts the fading voice and retries the primary solo.
    pipeline.abortFadingVoice();
    pipeline.open(44100, 2);
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(!pipeline.isCrossfading());
}

void TestPlaybackPipeline::testVolumeAndMuteFanOutToAllStreams()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{2});

    // setVolume() fans the linear gain out to both voice streams.
    pipeline.setVolume(0.5f);
    const float expectedGain = -std::log(1.0f - 0.5f) / kLog100;
    for (auto *stream : fakeSinkOf(pipeline)->liveStreams()) {
        QVERIFY(qAbs(stream->lastGain - expectedGain) < 1e-4f);
        QCOMPARE(stream->gainHistory.size(), 1);
    }

    // setMuted() fans out independently without overwriting stream gain.
    pipeline.setMuted(true);
    for (auto *stream : fakeSinkOf(pipeline)->liveStreams()) {
        QCOMPARE(stream->muteHistory, QList<bool>{true});
        QCOMPARE(stream->gainHistory.size(), 1);
        QVERIFY(qAbs(stream->lastGain - expectedGain) < 1e-4f);
    }

    pipeline.setMuted(false);
    for (auto *stream : fakeSinkOf(pipeline)->liveStreams()) {
        QCOMPARE(stream->muteHistory, (QList<bool>{true, false}));
        QCOMPARE(stream->gainHistory.size(), 1);
    }
}

void TestPlaybackPipeline::testExternalVolumeOnlyFromPrimaryMirroredToFading()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);

    // An external change on the fading stream is ignored.
    const float volumeBefore = pipeline.volume();
    liveStream(pipeline, 0)->emitExternalVolumeChanged(0.9f);
    QCOMPARE(pipeline.volume(), volumeBefore);

    // An external change on the current primary is adopted and mirrored to
    // the fading stream.
    liveStream(pipeline, 1)->emitExternalVolumeChanged(0.5f);
    const float expectedSlider = 1.0f - std::exp(-0.5f * kLog100);
    QVERIFY(qAbs(pipeline.volume() - expectedSlider) < 0.01f);
    QCOMPARE(liveStream(pipeline, 0)->gainHistory.size(), 1);
}

void TestPlaybackPipeline::testVolumeRestoreAdoptedOnlyForFirstStream()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);
    pipeline.open(44100, 2);

    // A startup externalVolumeRestored on the pipeline's first primary
    // stream is adopted.
    auto *first = liveStream(pipeline, 0);
    first->emitExternalVolumeRestored(0.5f);
    const float expectedSlider = 1.0f - std::exp(-0.5f * kLog100);
    QVERIFY(qAbs(pipeline.volume() - expectedSlider) < 0.01f);

    // A replacement stream must not re-restore an old per-node value over
    // the user's current volume.
    pipeline.open(44100, 2);
    liveStream(pipeline, 0)->emitExternalVolumeRestored(1.0f);
    QVERIFY(qAbs(pipeline.volume() - expectedSlider) < 0.01f);
}

void TestPlaybackPipeline::testStreamNameFansOutAndAppliesToNewStreams()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    pipeline.setStreamName(QStringLiteral("Elisa"));
    QCOMPARE(liveStream(pipeline, 0)->recordedName, QStringLiteral("Elisa"));

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    pipeline.setStreamName(QStringLiteral("Track 2"));

    // Live stream-name updates reach every active stream.
    QCOMPARE(liveStream(pipeline, 0)->recordedName, QStringLiteral("Track 2"));
    QCOMPARE(liveStream(pipeline, 1)->recordedName, QStringLiteral("Track 2"));

    // Closed streams disappear while the shared connection remains.
    pipeline.close();
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{1});
    QVERIFY(pipeline.probe());
}

void TestPlaybackPipeline::testFadingCompletionNotifiesByPipeIdentity()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 200ms, 1000ms);
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    // The incoming stream's first non-empty render releases the slow-load
    // hold so the outgoing fade can complete.
    QVERIFY(!liveStream(pipeline, 1)->pump(512).empty());

    Pipe<float> *endedPipe = nullptr;
    QString endedError;
    pipeline.setFadingVoiceEndedCallback([&](Pipe<float> *pipe, const QString &error) {
        endedPipe = pipe;
        endedError = error;
        // The fading voice is already destroyed before this callback fires;
        // the pipe owner drains any unconsumed tail before reusing the pipe.
        if (pipe) {
            pipe->consumer().drain();
        }
    });

    // Drain the outgoing fade to completion through its stream callback;
    // the completion is a queued event, so the loop must be spun.
    for (int i = 0; i < 40; ++i) {
        std::ignore = liveStream(pipeline, 0)->pump(2048);
        QCoreApplication::processEvents();
        if (!pipeline.isCrossfading()) {
            break;
        }
    }
    QVERIFY(!pipeline.isCrossfading());
    QCOMPARE(endedPipe, &pipeA);
    QVERIFY(endedError.isEmpty());

    // A fading-stream error also destroys the voice and notifies the player
    // by pipe identity so its decoder cannot remain blocked.
    std::ignore = pipeline.armCrossfade(&pipeB, &pipeA, 1000ms, 1000ms);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    QCOMPARE(fakeSinkOf(pipeline)->createAttempts(), 3);
    // The fading voice's stream (still the only other live stream) errors:
    // the error is not player-fatal, but destroys the voice and notifies by
    // pipe identity.
    liveStream(pipeline, 0)->emitError(QStringLiteral("stream died"));
    QVERIFY(!pipeline.isCrossfading());
    QCOMPARE(endedPipe, &pipeB);
    QCOMPARE(endedError, QStringLiteral("stream died"));
}

void TestPlaybackPipeline::testHardSwitchDestroysBothVoices()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<float> pipeC;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    QVERIFY(pipeline.isCrossfading());

    // A new source during an active crossfade is a hard switch: destroy both
    // voices, construct one new pending primary around the new pipe, and
    // open its stream when the format is known. Never a third voice.
    const auto epoch = pipeline.setPositionOffset(0ms, &pipeC, 0ms);
    QVERIFY(epoch > 0);
    QVERIFY(!pipeline.isCrossfading());
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{0});

    writeAll(pipeC, makeConstant(0.7f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    QCOMPARE(fakeSinkOf(pipeline)->createAttempts(), 3);
    QVERIFY(pipeline.isPrimaryPipe(&pipeC));

    const auto out = drainSpan(liveStream(pipeline, 0)->pump(2048));
    QCOMPARE(out.size(), size_t{2048});
    QVERIFY(qAbs(out[0] - 0.7f) < 1e-6f);

    // The old voices' streams were closed and destroyed with their voices;
    // only the new primary stream remains open.
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{1});
}

void TestPlaybackPipeline::testResetReinstatesPendingPrimary()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    QVERIFY(pipeline.isCrossfading());
    // After the arm, the incoming pipeB is the primary.
    QVERIFY(pipeline.isPrimaryPipe(&pipeB));

    // reset() destroys both voices after their callbacks are quiesced and
    // reinstates a fresh pending primary around the current primary pipe.
    pipeline.close();
    pipeline.reset();
    QVERIFY(!pipeline.isCrossfading());
    QVERIFY(pipeline.isPrimaryPipe(&pipeB));
    QVERIFY(!pipeline.isDeviceOpen());

    writeAll(pipeB, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.open(44100, 2);
    QVERIFY(pipeline.isDeviceOpen());
    QCOMPARE(liveStream(pipeline, 0)->sampleRate, 44100);
    const auto out = drainSpan(liveStream(pipeline, 0)->pump(2048));
    QCOMPARE(out.size(), size_t{2048});
    QVERIFY(qAbs(out[0] - 0.5f) < 1e-6f);
}

QTEST_GUILESS_MAIN(TestPlaybackPipeline)
#include "test_playbackpipeline.moc"
