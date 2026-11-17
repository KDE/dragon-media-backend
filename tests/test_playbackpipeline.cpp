/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtTest>

#include "fakesink.h"
#include "fft/pcmblock.h"
#include "player/pipe.h"
#include "sink/playbackpipeline.h"
#include "voice/voice.h"

#include <QCoreApplication>
#include <QSignalSpy>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>
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

private:
    struct ZombieCleanup {
        std::vector<Voice *> voices;
        std::vector<FakeAudioSinkStream *> streams;

        void adopt(Voice *voice, FakeAudioSinkStream *stream)
        {
            voices.push_back(voice);
            streams.push_back(stream);
        }

        ~ZombieCleanup()
        {
            for (auto *stream : streams) {
                if (stream) {
                    stream->failClose = false;
                    delete stream;
                }
            }
            for (auto *voice : voices) {
                if (voice) {
                    voice->clearPoisonForTestCleanup();
                    delete voice;
                }
            }
        }
    };

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
    void testQueuedFormatChangeDroppedAcrossResetAndFlush();
    void testInitialVolumeInitPassedToCreateStream();
    void testDropVoiceDetachesFftBeforeZombieClose();
    void testCloseStreamPoisonIsSticky();
    void testOpenResetSeekAfterPoisonReplacePrimary();
    void testDestructorWithPoisonedFadingVoiceLeaksInsteadOfFreeing();
    void testCrossfadeLinkOutlivesZombieFadingVoiceAcrossHardSwitch();
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
    QVERIFY(pipeline.close());
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
    QVERIFY(pipeline.close());
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
    QVERIFY(pipeline.close());
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

void TestPlaybackPipeline::testQueuedFormatChangeDroppedAcrossResetAndFlush()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);

    QSignalSpy formatSpy(&pipeline, &PlaybackPipeline::formatChangeRequired);
    QSignalSpy trackSpy(&pipeline, &PlaybackPipeline::trackStarted);

    // Part 1: Queued formatChangeRequired (and trackStarted) emitted before close()/reset()
    // must be dropped when processed afterwards.
    pipeline.open(44100, 2);
    const auto ep1 = pipeline.streamEpoch();
    writeAll(pipe, makeConstant(0.5f, 1024, kTestChannels));
    pipeline.samplesProduced(ep1, 2048);
    QVERIFY(pipeline.boundaryTrackStart(2, 48000, 2, 1000ms, ep1));

    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});
    QVERIFY(liveStream(pipeline, 0)->pump(2048).empty());

    QVERIFY(pipeline.close());
    pipeline.reset();
    QCoreApplication::processEvents();
    QVERIFY(formatSpy.isEmpty());
    QVERIFY(trackSpy.isEmpty());

    // Part 2: Queued formatChangeRequired emitted before a staged flush (setPositionOffset)
    // must likewise be discarded by the epoch check once processed.
    pipe.consumer().drain();
    pipeline.open(44100, 2);
    const auto ep2 = pipeline.streamEpoch();
    writeAll(pipe, makeConstant(0.5f, 1024, kTestChannels));
    pipeline.samplesProduced(ep2, 2048);
    QVERIFY(pipeline.boundaryTrackStart(3, 48000, 2, 1000ms, ep2));

    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});
    QVERIFY(liveStream(pipeline, 0)->pump(2048).empty());

    std::ignore = pipeline.setPositionOffset(500ms);
    QVERIFY(pipeline.isFlushPending());

    QCoreApplication::processEvents();
    QVERIFY(formatSpy.isEmpty());
    QVERIFY(trackSpy.isEmpty());
}

void TestPlaybackPipeline::testInitialVolumeInitPassedToCreateStream()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    pipeline.setAudioPipe(&pipeA);

    // Before setVolume() / setMuted(): linearGain must be std::nullopt so
    // PipeWire/PulseAudio can honor server stream-restore.
    pipeline.open(44100, 2);
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{1});
    QVERIFY(!liveStream(pipeline, 0)->initialVolumeInit.linearGain.has_value());
    QCOMPARE(liveStream(pipeline, 0)->initialVolumeInit.muted, false);
    QVERIFY(liveStream(pipeline, 0)->gainHistory.isEmpty());
    QVERIFY(liveStream(pipeline, 0)->muteHistory.isEmpty());

    // After setVolume() / setMuted(): reopening passes explicit initial volume & mute
    // directly into createStream(), without post-creation setGain/setMuted calls.
    QVERIFY(pipeline.close());
    pipeline.setVolume(0.5f);
    pipeline.setMuted(true);
    pipeline.open(44100, 2);

    const float expectedGain = -std::log(1.0f - 0.5f) / kLog100;
    QVERIFY(liveStream(pipeline, 0)->initialVolumeInit.linearGain.has_value());
    QVERIFY(qAbs(*liveStream(pipeline, 0)->initialVolumeInit.linearGain - expectedGain) < 1e-4f);
    QCOMPARE(liveStream(pipeline, 0)->initialVolumeInit.muted, true);
    QVERIFY(liveStream(pipeline, 0)->gainHistory.isEmpty());
    QVERIFY(liveStream(pipeline, 0)->muteHistory.isEmpty());

    // Crossfade incoming stream also receives the initial volume and mute at creation.
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    pipeline.samplesProduced(pipeline.streamEpoch(), static_cast<std::size_t>(kTestFrames) * kTestChannels);
    liveStream(pipeline, 0)->resume();
    std::ignore = liveStream(pipeline, 0)->pump(2048);

    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 500ms, 500ms);
    pipeline.open(48000, 2);
    QCOMPARE(fakeSinkOf(pipeline)->liveStreams().size(), size_t{2});
    QVERIFY(liveStream(pipeline, 1)->initialVolumeInit.linearGain.has_value());
    QVERIFY(qAbs(*liveStream(pipeline, 1)->initialVolumeInit.linearGain - expectedGain) < 1e-4f);
    QCOMPARE(liveStream(pipeline, 1)->initialVolumeInit.muted, true);
    QVERIFY(liveStream(pipeline, 1)->gainHistory.isEmpty());
    QVERIFY(liveStream(pipeline, 1)->muteHistory.isEmpty());
}

void TestPlaybackPipeline::testDropVoiceDetachesFftBeforeZombieClose()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<PcmBlock> fftPipe;
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    // Destroy the intentionally leaked zombie Voice and its released stream
    // last-declared / first-destroyed so ASan/LSan sees zero leaks and ~Voice
    // runs while pipes & FakeAudioSink live.
    ZombieCleanup zombieCleanup;

    pipeline.setAudioPipe(&pipeA);
    pipeline.setFftPipe(&fftPipe);
    pipeline.open(44100, 2);

    writeAll(pipeA, makeConstant(0.5f, 1024, kTestChannels));
    pipeline.samplesProduced(pipeline.streamEpoch(), 2048);
    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});
    QVERIFY(fftPipe.consumer().ready() > 0);
    fftPipe.consumer().drain();

    FakeAudioSinkStream *oldStream = liveStream(pipeline, 0);
    QPointer<FakeAudioSinkStream> oldStreamPtr(oldStream);
    auto *oldVoice = static_cast<Voice *>(&oldStream->streamRenderer());
    oldStream->failClose = true;

    // Hard switch invokes dropVoice(oldPrimary), which must detach m_fftPipe BEFORE
    // closeStream() fails and leaks the zombie voice + stream.
    const auto newEpoch = pipeline.setPositionOffset(0ms, &pipeB, 0ms);
    zombieCleanup.adopt(oldVoice, oldStream);
    QVERIFY2(!oldStreamPtr.isNull(), "Leaked zombie stream/voice should remain alive when closeStream() returns false");

    // Pump the stuck/leaked zombie stream again: it still renders PCM cleanly,
    // yet produces zero blocks into fftPipe because its FFT tap is already detached.
    writeAll(pipeA, makeConstant(0.25f, 1024, kTestChannels));
    QCOMPARE(oldStream->pump(2048).size(), size_t{2048});
    QCOMPARE(fftPipe.consumer().ready(), size_t{0});

    pipeline.open(44100, 2);
    writeAll(pipeB, makeConstant(0.75f, 1024, kTestChannels));
    pipeline.samplesProduced(newEpoch, 2048);
    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});
    QVERIFY(fftPipe.consumer().ready() > 0);
}

void TestPlaybackPipeline::testCloseStreamPoisonIsSticky()
{
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    ZombieCleanup zombieCleanup;
    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);
    pipeline.open(44100, 2);

    FakeAudioSinkStream *stream = liveStream(pipeline, 0);
    QPointer<FakeAudioSinkStream> streamPtr(stream);
    auto *voice = static_cast<Voice *>(&stream->streamRenderer());
    stream->failClose = true;

    QVERIFY(!pipeline.close());
    QVERIFY(voice->isPoisoned());
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(!streamPtr.isNull());

    // Poison is sticky: repeat closeStream() / close() calls must continue
    // returning false even though m_stream is now nullptr.
    QVERIFY(!voice->closeStream());
    QVERIFY(!pipeline.close());

    // When the pipeline later drops the poisoned primary, it leaks it once.
    zombieCleanup.adopt(voice, stream);
    pipeline.reset();
    QVERIFY(!streamPtr.isNull());
}

void TestPlaybackPipeline::testOpenResetSeekAfterPoisonReplacePrimary()
{
    // 1) open() after poison (pre-close failure): replaces poisoned primary,
    //    emits errorOccurred, never frees the poisoned voice, and subsequent
    //    open() creates a working stream on the fresh primary.
    {
        Pipe<float> pipe;
        PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
        ZombieCleanup zombieCleanup;
        pipeline.setAudioPipe(&pipe);
        pipeline.open(44100, 2);

        FakeAudioSinkStream *oldStream = liveStream(pipeline, 0);
        QPointer<FakeAudioSinkStream> oldStreamPtr(oldStream);
        auto *oldVoice = static_cast<Voice *>(&oldStream->streamRenderer());
        oldStream->failClose = true;

        QSignalSpy errorSpy(&pipeline, &PlaybackPipeline::errorOccurred);
        pipeline.open(48000, 2);
        zombieCleanup.adopt(oldVoice, oldStream);

        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!pipeline.isDeviceOpen());
        QVERIFY(pipeline.isPrimaryPipe(&pipe));
        QVERIFY(!oldStreamPtr.isNull());
        QVERIFY(oldVoice->isPoisoned());

        pipeline.open(48000, 2);
        QVERIFY(pipeline.isDeviceOpen());
        FakeAudioSinkStream *newStream = liveStream(pipeline, 0);
        QVERIFY(newStream != oldStream);
        QVERIFY(&newStream->streamRenderer() != oldVoice);
        writeAll(pipe, makeConstant(0.5f, 1024, kTestChannels));
        QCOMPARE(newStream->pump(2048).size(), size_t{2048});
    }

    // 2) reset() after poison: replaces primary without calling resetForTeardown()
    //    on the poisoned voice.
    {
        Pipe<float> pipe;
        PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
        ZombieCleanup zombieCleanup;
        pipeline.setAudioPipe(&pipe);
        pipeline.open(44100, 2);

        FakeAudioSinkStream *oldStream = liveStream(pipeline, 0);
        QPointer<FakeAudioSinkStream> oldStreamPtr(oldStream);
        auto *oldVoice = static_cast<Voice *>(&oldStream->streamRenderer());

        writeAll(pipe, makeConstant(0.5f, 1024, kTestChannels));
        QCOMPARE(oldStream->pump(2048).size(), size_t{2048});
        QVERIFY(oldVoice->clock().totalSamplesWritten() > 0);
        oldVoice->setPendingFadeIn(777ms);
        std::ignore = oldVoice->stageFlush(333ms);
        QVERIFY(oldVoice->isFlushPending());

        oldStream->failClose = true;
        QVERIFY(!pipeline.close());
        pipeline.reset();
        zombieCleanup.adopt(oldVoice, oldStream);

        QVERIFY(!oldStreamPtr.isNull());
        QVERIFY(oldVoice->isPoisoned());
        QVERIFY2(oldVoice->isFlushPending(), "resetForTeardown() must not run on a poisoned voice");
        QCOMPARE(oldVoice->pendingFadeIn(), 777ms);
        QVERIFY(oldVoice->clock().totalSamplesWritten() > 0);
        QVERIFY(pipeline.isPrimaryPipe(&pipe));
        QVERIFY(!pipeline.isDeviceOpen());

        pipeline.open(44100, 2);
        QVERIFY(pipeline.isDeviceOpen());
        QVERIFY(&liveStream(pipeline, 0)->streamRenderer() != oldVoice);
    }

    // 3) Paused/closed seek (setPositionOffset) after poison: replaces primary
    //    without calling runFlushQuiesced() on the poisoned voice.
    {
        Pipe<float> pipe;
        PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
        ZombieCleanup zombieCleanup;
        pipeline.setAudioPipe(&pipe);
        pipeline.open(44100, 2);

        FakeAudioSinkStream *oldStream = liveStream(pipeline, 0);
        QPointer<FakeAudioSinkStream> oldStreamPtr(oldStream);
        auto *oldVoice = static_cast<Voice *>(&oldStream->streamRenderer());
        const auto oldEpoch = oldVoice->clock().streamEpoch();

        writeAll(pipe, makeConstant(0.5f, 1024, kTestChannels));
        oldStream->pause();
        oldStream->failClose = true;

        const auto seekEpoch = pipeline.setPositionOffset(1234ms);
        zombieCleanup.adopt(oldVoice, oldStream);

        QVERIFY(seekEpoch > 0);
        QVERIFY(!oldStreamPtr.isNull());
        QVERIFY(oldVoice->isPoisoned());
        // runFlushQuiesced() would have updated positionOffset, drained the pipe,
        // and advanced the old voice's streamEpoch.
        QCOMPARE(oldVoice->clock().positionOffset(), 0ms);
        QCOMPARE(oldVoice->clock().streamEpoch(), oldEpoch);
        QCOMPARE(pipe.consumer().ready(), size_t{2048});

        QVERIFY(pipeline.isPrimaryPipe(&pipe));
        QCOMPARE(pipeline.position(), 1234ms);
        pipeline.open(44100, 2);
        QVERIFY(pipeline.isDeviceOpen());
        QVERIFY(&liveStream(pipeline, 0)->streamRenderer() != oldVoice);
    }
}

void TestPlaybackPipeline::testDestructorWithPoisonedFadingVoiceLeaksInsteadOfFreeing()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    ZombieCleanup zombieCleanup;

    QPointer<FakeAudioSinkStream> fadingStreamPtr;
    QPointer<FakeAudioSinkStream> primaryStreamPtr;
    Voice *fadingVoice = nullptr;

    {
        PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
        pipeline.setAudioPipe(&pipeA);
        pipeline.open(44100, 2);
        writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
        QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});

        std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
        pipeline.open(44100, 2);

        FakeAudioSinkStream *fadingStream = liveStream(pipeline, 0);
        fadingStreamPtr = fadingStream;
        fadingVoice = static_cast<Voice *>(&fadingStream->streamRenderer());
        primaryStreamPtr = liveStream(pipeline, 1);
        fadingStream->failClose = true;

        zombieCleanup.adopt(fadingVoice, fadingStream);
    }

    QVERIFY2(primaryStreamPtr.isNull(), "Unpoisoned primary voice should be destroyed by ~PlaybackPipeline()");
    QVERIFY2(!fadingStreamPtr.isNull(), "Poisoned fading voice/stream must be leaked by ~PlaybackPipeline()");
    QVERIFY(fadingVoice->isPoisoned());
    // Stuck callback can still safely pull from the leaked fading voice under ASan.
    QCOMPARE(fadingStreamPtr->pump(2048).size(), size_t{2048});
}

void TestPlaybackPipeline::testCrossfadeLinkOutlivesZombieFadingVoiceAcrossHardSwitch()
{
    Pipe<float> pipeA;
    Pipe<float> pipeB;
    Pipe<float> pipeC;
    PlaybackPipeline pipeline(std::make_unique<FakeAudioSink>());
    ZombieCleanup zombieCleanup;

    pipeline.setAudioPipe(&pipeA);
    pipeline.open(44100, 2);
    writeAll(pipeA, makeConstant(0.5f, kTestFrames, kTestChannels));
    QCOMPARE(liveStream(pipeline, 0)->pump(2048).size(), size_t{2048});

    // Arm crossfade: pipeB's primary voice shares the CrossfadeLink, while pipeA's
    // demoted fading voice holds a reference to it and is in slow-load hold
    // because pipeB has not rendered yet.
    std::ignore = pipeline.armCrossfade(&pipeA, &pipeB, 500ms, 500ms);
    pipeline.open(44100, 2);

    FakeAudioSinkStream *fadingStream = liveStream(pipeline, 0);
    QPointer<FakeAudioSinkStream> fadingStreamPtr(fadingStream);
    auto *fadingVoice = static_cast<Voice *>(&fadingStream->streamRenderer());
    QPointer<FakeAudioSinkStream> primaryBStreamPtr(liveStream(pipeline, 1));
    fadingStream->failClose = true;

    // Hard switch to pipeC: destroys pipeB's primary voice (the original link
    // owner) and leaks pipeA's zombie fading voice.
    std::ignore = pipeline.setPositionOffset(0ms, &pipeC, 0ms);
    zombieCleanup.adopt(fadingVoice, fadingStream);

    QVERIFY2(primaryBStreamPtr.isNull(), "Incoming primary voice B should be destroyed on hard switch");
    QVERIFY2(!fadingStreamPtr.isNull(), "Zombie fading voice A must remain alive");

    // Pump the leaked zombie fading voice after primary B was destroyed: it
    // reads CrossfadeLink::incomingStarted (slow-load hold) and writes
    // CrossfadeLink::outgoingActive on completion without UAF.
    QCOMPARE(fadingStream->pump(2048).size(), size_t{2048});
    pipeA.consumer().drain();
    pipeA.setProducerDone(true);
    QVERIFY(fadingStream->pump(2048).empty());
}

QTEST_GUILESS_MAIN(TestPlaybackPipeline)
#include "test_playbackpipeline.moc"
