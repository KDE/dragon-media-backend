/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "helpers/pa_sink_input_props.h"
#include "helpers/pw_node_props.h"
#include "pipe_test_utils.h"
#include "player/pipe.h"
#include "sink/playbackpipeline.h"
#include "sink/playbackpipelinefactory.h"
#include "sink/producerstate.h"
#include "sink/streamclock.h"
#include "voice/voice.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_version.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

class TestAudioOutput : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testConstruction();
    void testVolumeSetGet();
    void testMuteSetGet();
    void testVolumeMuteInteraction();
    void testVolumeChangedSignal();
    void testSetQueue();
    void testSetStreamNameSmoke();
    void testStreamNameServerRoundTrip();
    void testStreamNameLiveUpdate();
    void testSdlStreamNameHints();

    void testPositionMsCalculation();
    void testTotalSamplesWritten();

    void testReset();
    void testStopWithoutStart();
    void testStartStopLifecycle();
    void testMultipleStartStopCycles();
    void testAudioDataProcessing();
    void testPositionTrackingWithData();
    void testQueueBehavior();
    void testStartWhileAlreadyStarted();

    void testStopWithActiveCallbacks();
    void testRapidStartStopCycles();
    void testStopDuringStarvation();

    void testQueueReadyApi();
    void testFlushOpensGate();

    void testStartPaused();

    void testPauseResumeCycle();

    void testIsPausedBasic();
    void testIsPausedAfterStop();

    void testSeekWhilePaused();
    void testPositionStabilityDuringPause();
    void testPositionAdvancesSmoothlyDuringPlayback();

    void testVolumeChangeWhilePlaying();
    void testStartupExternalVolumeRestored();
    void testExternalVolumeChangePropagates();
    void testExternalMuteChangePropagates();

    void testDrainCallback();
    void testDrainRepeatCycles();
    void testBoundaryStaleEpochTagDiscarded();
    void testProducerStateUnit();
    void testSamplesProducedStaleEpochDiscarded();
    void testSamplesProducedConcurrentFlushRace();
    void testEosPublicationOrderRace();
    void testSdlPausedSeekAndCrossfadePauseCount();
    void testPulseAudioSecondStreamFailurePreservesFirstStream();

private:
    void fillQueue(Pipe<float> *pipe, const std::vector<float> &data);
    static bool waitForWrittenCounterStable(PlaybackPipeline *output);
    static QString currentSinkBackend();
    static bool findSinkInputByApplicationName(const QString &appName, uint32_t &sinkInputIndexOut);
    static bool setSinkInputVolume(uint32_t sinkInputIndex, int percent);
    static bool setSinkInputMute(uint32_t sinkInputIndex, bool mute);
    static bool findPwNodeByApplicationName(const QString &appName, uint32_t &nodeIdOut);
    static bool setPwNodeVolume(uint32_t nodeId, float linearGain);
    static bool setPwNodeMute(uint32_t nodeId, bool mute);
};

void TestAudioOutput::testConstruction()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);
    QVERIFY(!output->muted());
    QVERIFY(!output->isDeviceOpen());
    QVERIFY(output->isQueueReady());
    QCOMPARE(output->position(), 0ms);
    QCOMPARE(output->totalSamplesWritten(), 0);
}

void TestAudioOutput::testVolumeSetGet()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);

    output->setVolume(0.5f);
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setVolume(0.0f);
    QVERIFY(qAbs(output->volume() - 0.0f) < 0.01f);

    output->setVolume(1.5f);
    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);

    output->setVolume(1.0f);
    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);
}

void TestAudioOutput::testMuteSetGet()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(!output->muted());

    output->setMuted(true);
    QVERIFY(output->muted());

    output->setMuted(false);
    QVERIFY(!output->muted());

    output->setMuted(true);
    QVERIFY(output->muted());
    output->setMuted(true);
    QVERIFY(output->muted());
}

void TestAudioOutput::testVolumeMuteInteraction()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    output->setVolume(0.5f);
    output->setMuted(true);
    QVERIFY(output->muted());
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setMuted(false);
    QVERIFY(!output->muted());
    QVERIFY(qAbs(output->volume() - 0.5f) < 0.01f);

    output->setMuted(true);
    output->setVolume(0.3f);
    QVERIFY(output->muted());
    QVERIFY(qAbs(output->volume() - 0.3f) < 0.01f);
}

void TestAudioOutput::testVolumeChangedSignal()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);
    QSignalSpy spy(output.get(), &PlaybackPipeline::volumeChanged);

    output->setVolume(0.7f);
    QVERIFY(spy.count() > 0);

    int countBefore = spy.count();
    output->setVolume(0.7f);
    QVERIFY2(spy.count() == countBefore, "Setting same volume should not emit signal");

    QSignalSpy muteSpy(output.get(), &PlaybackPipeline::mutedChanged);
    output->setMuted(true);
    QVERIFY(muteSpy.count() > 0);
}

void TestAudioOutput::testSetQueue()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);
    Pipe<float> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(!output->isDeviceOpen());

    output->setAudioPipe(nullptr);
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testSetStreamNameSmoke()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    output->setStreamName("Test Audio"_L1);
    output->setStreamName(""_L1);
    output->setStreamName("Longer Name With Spaces"_L1);
    QVERIFY2(!output->isDeviceOpen(), "setStreamName should not open the audio device");
}

void TestAudioOutput::testStreamNameServerRoundTrip()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonsdlaudiosink"_s && backend != u"dragonpulseaudiosink"_s && backend != u"dragonpipewireaudiosink"_s) {
        QSKIP("Stream-name server round trip requires the SDL, PulseAudio, or PipeWire backend");
    }

    const QString uniqueTag = u"DragonStreamNameTest_%1"_s.arg(QCoreApplication::applicationPid());
    const QString priorDisplayName = QGuiApplication::applicationDisplayName();
    QGuiApplication::setApplicationDisplayName(uniqueTag);
    auto nameGuard = qScopeGuard([&priorDisplayName]() {
        QGuiApplication::setApplicationDisplayName(priorDisplayName);
    });

    const QString desktopTag = u"dev.eean.dragontest"_s;
    const QString priorDesktopFile = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(desktopTag);
    auto desktopGuard = qScopeGuard([&priorDesktopFile]() {
        QGuiApplication::setDesktopFileName(priorDesktopFile);
    });

    const bool viaPulse = backend == u"dragonpulseaudiosink"_s;
    const bool viaSdl = backend == u"dragonsdlaudiosink"_s;
    auto serverProps = [uniqueTag, viaPulse]() -> std::optional<QJsonObject> {
        QJsonObject props;
        const bool found =
            viaPulse ? PaSinkInputProps::sinkInputPropsByApplicationName(uniqueTag, props) : PwNodeProps::nodePropsByApplicationName(uniqueTag, props);
        if (!found) {
            return std::nullopt;
        }
        return props;
    };
    auto serverProp = [&serverProps](const QString &key) -> std::optional<QString> {
        const auto props = serverProps();
        if (!props) {
            return std::nullopt;
        }
        return props->value(key).toString();
    };

    {
        auto fresh = createPlaybackPipeline();
        QVERIFY(fresh);

        Pipe<float> freshPipe(65536);
        fresh->setAudioPipe(&freshPipe);

        fresh->open(44100, 2);
        QVERIFY(fresh->isDeviceOpen());

        fillQueue(&freshPipe, std::vector<float>(8192, 0.5f));
        QTest::qWait(300);

        const bool visible = QTest::qWaitFor(
            [&serverProps]() {
                return serverProps().has_value();
            },
            5000);
        if (!visible) {
            QVERIFY(fresh->close());
            if (backend == u"dragonsdlaudiosink"_s) {
                QSKIP("SDL stream not visible in PipeWire (SDL audio not using its PipeWire driver)");
            }
            QFAIL("no stream with the test application name visible on the audio server");
        }

        QVERIFY2(QTest::qWaitFor(
                     [&serverProp, &uniqueTag]() {
                         return serverProp(u"media.name"_s) == uniqueTag;
                     },
                     5000),
                 "a fresh stream with no stored name should fall back to the application name for media.name");

        QVERIFY(fresh->close());
        QVERIFY2(QTest::qWaitFor(
                     [&serverProps]() {
                         return !serverProps().has_value();
                     },
                     5000),
                 "a closed stream should disappear from the audio server");
    }

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->setStreamName(u"Dragon Seeded Track"_s);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(16384, 0.5f));
    QTest::qWait(300);

    QVERIFY2(QTest::qWaitFor(
                 [&serverProp]() {
                     return serverProp(u"media.name"_s) == u"Dragon Seeded Track"_s;
                 },
                 5000),
             "name set before open() should be visible as media.name on the stream");

    const QString iconKey = viaPulse ? u"application.icon_name"_s : u"application.icon-name"_s;
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp, iconKey, desktopTag]() {
                     return serverProp(iconKey) == desktopTag;
                 },
                 5000),
             "application icon name should be derived from the desktop file name");
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp, desktopTag]() {
                     return serverProp(u"application.id"_s) == desktopTag;
                 },
                 5000),
             "application id should be derived from the desktop file name");

    output->setStreamName(QString());
    if (viaSdl) {
        QVERIFY2(QTest::qWaitFor(
                     [&serverProp]() {
                         return serverProp(u"media.name"_s) == u"Dragon Seeded Track"_s;
                     },
                     5000),
                 "an empty stream name on an open SDL stream should keep the previous media.name visible");
    } else {
        QVERIFY2(QTest::qWaitFor(
                     [&serverProp, &uniqueTag]() {
                         return serverProp(u"media.name"_s) == uniqueTag;
                     },
                     5000),
                 "an empty stream name should reset media.name to the application name");
    }

    QVERIFY(output->close());
}

void TestAudioOutput::testStreamNameLiveUpdate()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonsdlaudiosink"_s && backend != u"dragonpulseaudiosink"_s && backend != u"dragonpipewireaudiosink"_s) {
        QSKIP("Live stream-name updates require the SDL, PulseAudio, or PipeWire backend");
    }
    const bool viaPulse = backend == u"dragonpulseaudiosink"_s;
    const bool viaSdl = backend == u"dragonsdlaudiosink"_s;
    if (viaSdl && SDL_GetVersion() < SDL_VERSIONNUM(3, 6, 0)) {
        const int linkedSdlVersion = SDL_GetVersion();
        QSKIP(u"live stream-name updates require SDL 3.6.0+; this SDL is %1.%2.%3"_s.arg(SDL_VERSIONNUM_MAJOR(linkedSdlVersion))
                  .arg(SDL_VERSIONNUM_MINOR(linkedSdlVersion))
                  .arg(SDL_VERSIONNUM_MICRO(linkedSdlVersion))
                  .toUtf8()
                  .constData());
    }

    const QString uniqueTag = u"DragonStreamNameLive_%1"_s.arg(QCoreApplication::applicationPid());
    const QString priorDisplayName = QGuiApplication::applicationDisplayName();
    QGuiApplication::setApplicationDisplayName(uniqueTag);
    auto nameGuard = qScopeGuard([&priorDisplayName]() {
        QGuiApplication::setApplicationDisplayName(priorDisplayName);
    });

    auto serverProps = [uniqueTag, viaPulse]() -> std::optional<QJsonObject> {
        QJsonObject props;
        const bool found =
            viaPulse ? PaSinkInputProps::sinkInputPropsByApplicationName(uniqueTag, props) : PwNodeProps::nodePropsByApplicationName(uniqueTag, props);
        if (!found) {
            return std::nullopt;
        }
        return props;
    };
    auto serverProp = [&serverProps](const QString &key) -> std::optional<QString> {
        const auto props = serverProps();
        if (!props) {
            return std::nullopt;
        }
        return props->value(key).toString();
    };

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(16384, 0.5f));
    QTest::qWait(300);

    if (!QTest::qWaitFor(
            [&serverProps]() {
                return serverProps().has_value();
            },
            5000)) {
        QVERIFY(output->close());
        if (viaSdl) {
            QSKIP("SDL stream not visible in PipeWire (SDL audio not using its PipeWire driver)");
        }
        QFAIL("no stream with the test application name visible on the audio server");
    }

    output->setStreamName(u"Dragon Live Track"_s);
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp]() {
                     return serverProp(u"media.name"_s) == u"Dragon Live Track"_s;
                 },
                 5000),
             "setStreamName() on an open stream should update media.name without a reconnect");

    output->setStreamName(QString());
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp, &uniqueTag]() {
                     return serverProp(u"media.name"_s) == uniqueTag;
                 },
                 5000),
             "an empty stream name should reset media.name to the application name");

    QVERIFY(output->close());
}

void TestAudioOutput::testSdlStreamNameHints()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonsdlaudiosink"_s) {
        QSKIP("SDL hint assertions require the SDL backend");
    }

    const QString desktopTag = u"dev.eean.dragontest"_s;
    const QString priorDesktopFile = QGuiApplication::desktopFileName();
    QGuiApplication::setDesktopFileName(desktopTag);
    auto desktopGuard = qScopeGuard([&priorDesktopFile]() {
        QGuiApplication::setDesktopFileName(priorDesktopFile);
    });

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QCOMPARE(QString::fromUtf8(SDL_GetHint(SDL_HINT_AUDIO_DEVICE_APP_ICON_NAME)), desktopTag);

    output->setStreamName(u"Dragon Hint Track"_s);
    QCOMPARE(QString::fromUtf8(SDL_GetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME)), u"Dragon Hint Track"_s);

    output->setStreamName(QString());
    QVERIFY(!SDL_GetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME));

    output->setStreamName(u"Dragon Hint Track Two"_s);
    QCOMPARE(QString::fromUtf8(SDL_GetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME)), u"Dragon Hint Track Two"_s);
}

void TestAudioOutput::testPositionMsCalculation()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QVERIFY(output->position() == 0ms);
    QVERIFY(output->totalSamplesWritten() == 0);

    std::ignore = output->setPositionOffset(1000ms);
    QVERIFY2(output->position() == 1000ms, "position() should reflect the offset set via setPositionOffset");
}

void TestAudioOutput::testTotalSamplesWritten()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(output->totalSamplesWritten() == 0);

    output->reset();
    QVERIFY(output->totalSamplesWritten() == 0);
}

void TestAudioOutput::testReset()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    QVERIFY(output->close());
    output->reset();
    QVERIFY(output->position() == 0ms);
    QVERIFY(output->totalSamplesWritten() == 0);
}

void TestAudioOutput::testStopWithoutStart()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testStartStopLifecycle()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    QTest::qWait(100);

    QVERIFY(output->close());

    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->position() == 0ms);
}

void TestAudioOutput::testMultipleStartStopCycles()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 3; ++i) {
        output->open(44100, 2);
        QTest::qWait(50);
        QVERIFY(output->close());
        output->reset();
    }

    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testAudioDataProcessing()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    std::vector<float> audioData(4096, 0.5f);
    fillQueue(&pipe, audioData);

    output->open(44100, 2);

    QTest::qWait(50);
    fillQueue(&pipe, std::vector<float>(2048, 0.3f));

    QTest::qWait(100);

    qint64 samples = output->totalSamplesWritten();
    QVERIFY2(samples > 0, qPrintable(u"Expected some samples to be processed, got %1"_s.arg(samples)));

    QVERIFY(output->close());
}

void TestAudioOutput::testPositionTrackingWithData()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    std::vector<float> oneSecond(44100, 0.5f);
    fillQueue(&pipe, oneSecond);

    QTest::qWait(1500);

    const std::chrono::milliseconds posMs = output->position();
    QVERIFY2(posMs >= 100ms && posMs <= 800ms,
             qPrintable(u"Expected position in [100, 800]ms after feeding 1s of audio (44100 stereo), got %1 ms"_s.arg(posMs.count())));

    QVERIFY(output->close());
    output->reset();

    QVERIFY(output->position() == 0ms);
}

void TestAudioOutput::testQueueBehavior()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(!output->isDeviceOpen());

    auto output2 = createPlaybackPipeline();
    if (!output2)
        QSKIP("No audio sink available");

    Pipe<float> pipe2(65536);
    output2->setAudioPipe(&pipe2);
    output2->open(44100, 2);
    QVERIFY2(output2->isDeviceOpen(), "Second sink device should be open after open()");

    fillQueue(&pipe2, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    QVERIFY2(output2->totalSamplesWritten() > 0, "Second sink should have consumed samples from pipe");

    QVERIFY(output2->close());
    QVERIFY2(!output2->isDeviceOpen(), "Second sink should be closed after close()");
}

void TestAudioOutput::testStartWhileAlreadyStarted()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    output->open(48000, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(48000, 2));

    QVERIFY(output->close());
}

void TestAudioOutput::testStopWithActiveCallbacks()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(8192, 0.5f));

    QTest::qWait(20);

    fillQueue(&pipe, std::vector<float>(4096, 0.3f));

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->position() == 0ms);
}

void TestAudioOutput::testRapidStartStopCycles()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 10; ++i) {
        output->open(44100, 2);
        QVERIFY(output->isDeviceOpen());

        fillQueue(&pipe, std::vector<float>(2048, 0.5f));

        QTest::qWait(5);

        fillQueue(&pipe, std::vector<float>(2048, 0.3f));

        QVERIFY(output->close());
        QVERIFY(!output->isDeviceOpen());
        output->reset();
    }

    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testStopDuringStarvation()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->position() == 0ms);
}

void TestAudioOutput::testQueueReadyApi()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QVERIFY(output->isQueueReady());

    output->closeGate();
    QVERIFY(!output->isQueueReady());

    output->reopenIfIdle();
    QVERIFY(output->isQueueReady());

    output->reopenIfIdle();
    QVERIFY(output->isQueueReady());
    output->closeGate();
    QVERIFY(!output->isQueueReady());
    output->closeGate();
    QVERIFY(!output->isQueueReady());
}

void TestAudioOutput::testFlushOpensGate()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(50);

    output->closeGate();
    QVERIFY(!output->isQueueReady());

    std::ignore = output->setPositionOffset(0ms);

    QTest::qWait(100);

    QVERIFY2(output->isQueueReady(), "SDL callback should have processed flush and opened the gate");

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testStartPaused()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();

    QVERIFY(output->isPaused());
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    QCOMPARE(output->position(), 0ms);
    QCOMPARE(output->totalSamplesWritten(), 0);

    output->resume();
    QVERIFY(!output->isPaused());
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testPauseResumeCycle()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    const std::chrono::milliseconds posBefore = output->position();
    const qint64 writtenBefore = output->totalSamplesWritten();

    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->isPaused());

    QTest::qWait(200);

    QCOMPARE(output->position(), posBefore);

    const qint64 writtenAfterPause = output->totalSamplesWritten();
    QVERIFY2(writtenAfterPause <= writtenBefore + 2048, "Pause should stop or significantly reduce sample consumption");

    QTest::qWait(200);
    QCOMPARE(output->totalSamplesWritten(), writtenAfterPause);

    output->resume();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    QVERIFY(output->close());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testIsPausedBasic()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QVERIFY(!output->isPaused());

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isPaused());

    output->resume();
    QVERIFY(!output->isPaused());

    QVERIFY(output->close());
}

void TestAudioOutput::testIsPausedAfterStop()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();
    QVERIFY(output->isPaused());

    QVERIFY(output->close());
    QVERIFY(!output->isPaused());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testSeekWhilePaused()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    output->pause();
    QVERIFY(output->isPaused());

    QVERIFY(pipe.consumer().ready() == 0);
    fillQueue(&pipe, std::vector<float>(8192, 0.5f));
    const size_t queueSizeBeforeSeek = pipe.consumer().ready();
    QVERIFY2(queueSizeBeforeSeek > 0, "Queue should have stale samples before seek");

    output->closeGate();
    QVERIFY(!output->isQueueReady());

    const std::chrono::milliseconds seekTarget{30000};
    std::ignore = output->setPositionOffset(seekTarget);

    const std::chrono::milliseconds posAfterSeek = output->position();
    QCOMPARE(posAfterSeek, seekTarget);

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    QVERIFY(output->isQueueReady());

    output->resume();
    QVERIFY(!output->isPaused());

    QVERIFY(output->close());
}

void TestAudioOutput::testPositionStabilityDuringPause()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    fillQueue(&pipe, std::vector<float>(32768, 0.5f));
    QTest::qWait(100);

    output->pause();

    std::chrono::milliseconds posAtPause{};
    bool stable = false;
    for (int attempt = 0; attempt < 20 && !stable; ++attempt) {
        QTest::qWait(100);
        posAtPause = output->position();
        QTest::qWait(100);
        stable = output->position() == posAtPause;
    }
    QVERIFY2(stable, "Position must freeze after pause");

    for (int i = 0; i < 3; ++i) {
        QTest::qWait(100);
        QCOMPARE(output->position(), posAtPause);
    }

    output->resume();
    QTest::qWait(100);

    const std::chrono::milliseconds posAfterResume = output->position();
    QVERIFY2(posAfterResume >= posAtPause, "Position must not go backward after resume from pause");

    QVERIFY(output->close());
}

void TestAudioOutput::testPositionAdvancesSmoothlyDuringPlayback()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(1 << 19);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    const std::vector<float> quarterSecond(22050, 0.5f);
    fillQueue(&pipe, quarterSecond);

    QTest::qWait(300);

    QVector<std::chrono::milliseconds> samples;
    samples.reserve(25);
    for (int i = 0; i < 25; ++i) {
        fillQueue(&pipe, quarterSecond);
        QTest::qWait(100);
        samples.push_back(output->position());
    }

    QVERIFY(output->close());

    for (qsizetype i = 1; i < samples.size(); ++i) {
        QVERIFY2(samples[i] >= samples[i - 1] - 50ms,
                 qPrintable(u"Position must not skip backwards (sample %1: %2 -> %3)"_s.arg(i).arg(samples[i - 1].count()).arg(samples[i].count())));
        QVERIFY2(samples[i] - samples[i - 1] <= 250ms,
                 qPrintable(u"Position must not leap forward in a burst (sample %1: %2 -> %3)"_s.arg(i).arg(samples[i - 1].count()).arg(samples[i].count())));
    }

    for (qsizetype i = 5; i < samples.size(); ++i) {
        QVERIFY2(samples[i] - samples[i - 5] >= 200ms,
                 qPrintable(u"Position must not stall for 500ms or longer (sample %1: %2 -> %3)"_s.arg(i).arg(samples[i - 5].count()).arg(samples[i].count())));
    }
}

void TestAudioOutput::fillQueue(Pipe<float> *pipe, const std::vector<float> &data)
{
    pipe->producer().write(data, std::stop_token{});
}

bool TestAudioOutput::waitForWrittenCounterStable(PlaybackPipeline *output)
{
    qint64 previous = output->totalSamplesWritten();
    for (int attempt = 0; attempt < 20; ++attempt) {
        QTest::qWait(150);
        const qint64 current = output->totalSamplesWritten();
        if (current == previous) {
            QTest::qWait(150);
            return output->totalSamplesWritten() == current;
        }
        previous = current;
    }
    return false;
}

QString TestAudioOutput::currentSinkBackend()
{
    return qEnvironmentVariable("DRAGON_AUDIO_SINK");
}

bool TestAudioOutput::findSinkInputByApplicationName(const QString &appName, uint32_t &sinkInputIndexOut)
{
    QProcess pactl;
    pactl.start(u"pactl"_s, QStringList{u"--format"_s, u"json"_s, u"list"_s, u"sink-inputs"_s});
    if (!pactl.waitForFinished(5000)) {
        return false;
    }
    const QByteArray output = pactl.readAllStandardOutput();
    if (output.isEmpty()) {
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        return false;
    }

    const QJsonArray root = doc.array();
    for (const QJsonValue &entry : root) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject obj = entry.toObject();
        const QJsonValue props = obj.value(u"properties"_s);
        if (!props.isObject()) {
            continue;
        }
        const QString name = props.toObject().value(u"application.name"_s).toString();
        if (name.compare(appName, Qt::CaseInsensitive) == 0) {
            const QJsonValue idxVal = obj.value(u"index"_s);
            if (!idxVal.isDouble()) {
                continue;
            }
            sinkInputIndexOut = static_cast<uint32_t>(idxVal.toDouble());
            return true;
        }
    }
    return false;
}

bool TestAudioOutput::setSinkInputVolume(uint32_t sinkInputIndex, int percent)
{
    QProcess pactl;
    pactl.start(u"pactl"_s, QStringList{u"set-sink-input-volume"_s, QString::number(sinkInputIndex), QString::number(percent) + u"%"_s});
    return pactl.waitForFinished(3000) && pactl.exitCode() == 0;
}

bool TestAudioOutput::findPwNodeByApplicationName(const QString &appName, uint32_t &nodeIdOut)
{
    QProcess pwcli;
    pwcli.start(u"pw-cli"_s, QStringList{u"ls"_s, u"Node"_s});
    if (!pwcli.waitForFinished(5000)) {
        return false;
    }
    const QString output = QString::fromUtf8(pwcli.readAllStandardOutput());

    QRegularExpression idRe(u"^\\s*id (\\d+), type PipeWire:Interface:Node"_s);
    QRegularExpression appRe(u"application.name = \"%1\""_s.arg(QRegularExpression::escape(appName)));

    std::optional<uint32_t> currentNode;
    const QStringList lines = output.split(u'\n');
    for (const QString &line : lines) {
        const QRegularExpressionMatch idMatch = idRe.match(line);
        if (idMatch.hasMatch()) {
            bool ok = false;
            const uint32_t id = idMatch.captured(1).toUInt(&ok);
            if (ok) {
                currentNode = id;
            }
            continue;
        }
        if (currentNode.has_value() && appRe.match(line).hasMatch()) {
            nodeIdOut = *currentNode;
            return true;
        }
    }
    return false;
}

bool TestAudioOutput::setPwNodeVolume(uint32_t nodeId, float linearGain)
{
    QProcess pwcli;
    const QString pod = u"{ channelVolumes = [ %1, %1 ] }"_s.arg(linearGain);
    pwcli.start(u"pw-cli"_s, QStringList{u"set-param"_s, QString::number(nodeId), u"Props"_s, pod});
    return pwcli.waitForFinished(3000) && pwcli.exitCode() == 0;
}

bool TestAudioOutput::setSinkInputMute(uint32_t sinkInputIndex, bool mute)
{
    QProcess pactl;
    pactl.start(u"pactl"_s, QStringList{u"set-sink-input-mute"_s, QString::number(sinkInputIndex), mute ? u"1"_s : u"0"_s});
    return pactl.waitForFinished(3000) && pactl.exitCode() == 0;
}

bool TestAudioOutput::setPwNodeMute(uint32_t nodeId, bool mute)
{
    QProcess pwcli;
    const QString pod = mute ? u"{ mute = true }"_s : u"{ mute = false }"_s;
    pwcli.start(u"pw-cli"_s, QStringList{u"set-param"_s, QString::number(nodeId), u"Props"_s, pod});
    return pwcli.waitForFinished(3000) && pwcli.exitCode() == 0;
}

void TestAudioOutput::testDrainCallback()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QSignalSpy drainSpy(output.get(), &PlaybackPipeline::drained);
    QCOMPARE(drainSpy.count(), 0);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    // Feed a small amount of audio enough to fill the backend buffer so
    // the drain path is meaningful, but small enough to drain quickly.
    // 8820 stereo floats = 100ms at 44100 Hz / 2 channels
    fillQueue(&pipe, std::vector<float>(8820, 0.5f));

    // Let the audio callback consume the data from the pipe.
    const bool consumed = QTest::qWaitFor(
        [&]() {
            return pipe.consumer().ready() == 0;
        },
        3000);
    QVERIFY2(consumed, "Audio callback should have consumed all pipe data before drain");

    QVERIFY(output->boundaryEndOfStream(output->streamEpoch()));

    const bool drained = QTest::qWaitFor(
        [&]() {
            return drainSpy.count() > 0;
        },
        5000);
    QVERIFY2(drained, "drained() signal should fire after the in-band end of stream and pipe empty");

    QVERIFY(!output->boundaryEndOfStream(output->streamEpoch()));

    std::ignore = output->setPositionOffset(std::chrono::milliseconds{0});
    output->resume();

    fillQueue(&pipe, std::vector<float>(8820, 0.5f));
    const bool consumedAgain = QTest::qWaitFor(
        [&]() {
            return pipe.consumer().ready() == 0;
        },
        3000);
    QVERIFY2(consumedAgain, "Audio callback should have consumed all pipe data before the second drain");

    QVERIFY(output->boundaryEndOfStream(output->streamEpoch()));

    const qint64 countAfterFirstDrain = drainSpy.count();
    const bool drainedAgain = QTest::qWaitFor(
        [&]() {
            return drainSpy.count() > countAfterFirstDrain;
        },
        5000);
    QVERIFY2(drainedAgain, "drained() signal should fire again after a flush re-arms the end of stream");

    QTest::qWait(150);
    QCOMPARE(drainSpy.count(), countAfterFirstDrain + 1);

    QVERIFY(output->close());
}

void TestAudioOutput::testDrainRepeatCycles()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QSignalSpy drainSpy(output.get(), &PlaybackPipeline::drained);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    constexpr int kCycles = 10;
    for (int i = 0; i < kCycles; ++i) {
        fillQueue(&pipe, std::vector<float>(8820, 0.5f));
        const bool consumed = QTest::qWaitFor(
            [&]() {
                return pipe.consumer().ready() == 0;
            },
            3000);
        QVERIFY2(consumed, qPrintable(u"cycle %1: audio data not consumed"_s.arg(i)));

        QVERIFY2(output->boundaryEndOfStream(output->streamEpoch()), qPrintable(u"cycle %1: end of stream could not be armed"_s.arg(i)));

        const qint64 countBefore = drainSpy.count();
        const bool drained = QTest::qWaitFor(
            [&]() {
                return drainSpy.count() > countBefore;
            },
            5000);
        QVERIFY2(drained, qPrintable(u"cycle %1: drained() did not fire"_s.arg(i)));

        QTest::qWait(50);
        QCOMPARE(drainSpy.count(), countBefore + 1);

        std::ignore = output->setPositionOffset(std::chrono::milliseconds{0});
        output->resume();
    }

    QVERIFY(output->close());
}

void TestAudioOutput::testBoundaryStaleEpochTagDiscarded()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QSignalSpy trackSpy(output.get(), &PlaybackPipeline::trackStarted);
    QCOMPARE(trackSpy.count(), 0);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    const std::uint64_t epoch = output->streamEpoch();
    fillQueue(&pipe, std::vector<float>(8820, 0.5f));
    output->samplesProduced(epoch, 8820);
    QVERIFY(output->boundaryTrackStart(7, 44100, 2, {}, epoch));

    QTRY_COMPARE_WITH_TIMEOUT(trackSpy.count(), 1, 5000);

    // A boundary queued under the old epoch before a flush supersedes the stream
    // must be discarded by the consumer's epoch check after the flush.
    QVERIFY(output->boundaryTrackStart(8, 44100, 2, {}, epoch));
    const std::uint64_t newEpoch = output->setPositionOffset(std::chrono::milliseconds{0});
    QVERIFY2(newEpoch > epoch, "flush should bump the stream epoch");
    QTRY_VERIFY_WITH_TIMEOUT(!output->isFlushPending(), 3000);

    fillQueue(&pipe, std::vector<float>(8820, 0.5f));
    output->samplesProduced(newEpoch, 8820);
    // A late boundary stamped with the stale epoch after the flush is also discarded.
    QVERIFY(output->boundaryTrackStart(9, 44100, 2, {}, epoch));

    const bool consumed = QTest::qWaitFor(
        [&]() {
            return pipe.consumer().ready() == 0;
        },
        3000);
    QVERIFY2(consumed, "Audio callback should consume the flushed stream's data");

    QTest::qWait(300);
    QCOMPARE(trackSpy.count(), 1);

    QVERIFY(output->close());
}

void TestAudioOutput::testProducerStateUnit()
{
    ProducerState state{1};
    QCOMPARE(state.epoch(), 1);
    QCOMPARE(state.samples(), 0);

    // Matching epoch adds samples
    QVERIFY(state.addSamples(1, 100));
    QCOMPARE(state.samples(), 100);
    QVERIFY(state.addSamples(1, 200));
    QCOMPARE(state.samples(), 300);

    // Mismatched / stale epoch is rejected and does not alter sample count
    QVERIFY(!state.addSamples(2, 50));
    QVERIFY(!state.addSamples(0, 50));
    QCOMPARE(state.samples(), 300);
    QCOMPARE(state.epoch(), 1);

    // Bumping epoch increments epoch and resets sample count to 0 atomically
    const auto newEpoch = state.bumpEpoch();
    QCOMPARE(newEpoch, 2);
    QCOMPARE(state.epoch(), 2);
    QCOMPARE(state.samples(), 0);

    // Adding samples with the old epoch (1) is rejected
    QVERIFY(!state.addSamples(1, 50));
    QCOMPARE(state.samples(), 0);

    // Adding samples with the new epoch (2) succeeds
    QVERIFY(state.addSamples(2, 400));
    QCOMPARE(state.samples(), 400);

    // Test epoch wrap-around at kEpochMask: wraps to 1 (never 0)
    state.reset(ProducerState::kEpochMask, 1000);
    QCOMPARE(state.epoch(), ProducerState::kEpochMask);
    QCOMPARE(state.samples(), 1000);
    const auto wrappedEpoch = state.bumpEpoch();
    QCOMPARE(wrappedEpoch, 1);
    QCOMPARE(state.epoch(), 1);
    QCOMPARE(state.samples(), 0);
}

void TestAudioOutput::testSamplesProducedStaleEpochDiscarded()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    const std::uint64_t initialEpoch = output->streamEpoch();
    QCOMPARE(output->producerSamples(), 0);

    // Producing samples in current epoch advances producerSamples
    output->samplesProduced(initialEpoch, 1000);
    QCOMPARE(output->producerSamples(), 1000);

    // Flush supersedes the epoch and resets producerSamples to 0
    std::ignore = output->setPositionOffset(std::chrono::milliseconds{0});
    QTRY_VERIFY2(output->streamEpoch() > initialEpoch, "flush should bump the stream epoch");
    QCOMPARE(output->producerSamples(), 0);

    // Samples produced with the stale initialEpoch must be discarded and NOT pollute the new epoch
    output->samplesProduced(initialEpoch, 2000);
    QCOMPARE(output->producerSamples(), 0);

    // Samples produced with the current epoch must be accounted
    const std::uint64_t currentEpoch = output->streamEpoch();
    output->samplesProduced(currentEpoch, 500);
    QCOMPARE(output->producerSamples(), 500);

    QVERIFY(output->close());
}

void TestAudioOutput::testSamplesProducedConcurrentFlushRace()
{
    ProducerState state{1};
    std::atomic<bool> stop{false};

    // Thread A: repeatedly loads epoch and calls addSamples
    std::jthread producerThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            const auto ep = state.epoch();
            state.addSamples(ep, 10);
        }
    });

    // Thread B: repeatedly flushes / bumps epoch
    std::jthread flushThread([&]() {
        for (int i = 0; i < 500; ++i) {
            state.bumpEpoch();
            std::this_thread::yield();
        }
    });

    flushThread.join();
    stop.store(true, std::memory_order_relaxed);
    producerThread.join();

    // After stopping producer and performing one final flush,
    // sample count must be exactly 0, and state must remain consistent.
    state.bumpEpoch();
    QCOMPARE(state.samples(), 0);
}

void TestAudioOutput::testEosPublicationOrderRace()
{
    Pipe<float> pipe(65536);
    Voice voice{&pipe, 1, Voice::Notifiers{}};
    voice.setFormat(44100, 2);
    std::ignore = voice.publishInitialStream(0ms);
    StreamClock &clock = voice.clock();

    std::atomic<bool> stop{false};
    std::atomic<int> staleEosObserved{0};

    std::jthread consumerThread([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (clock.endOfStreamPending() && clock.producerSamples() == 10000 && clock.streamSamplesRead() == 100) {
                staleEosObserved.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    const std::vector<float> hundredSamples(100, 0.5f);
    for (int i = 0; i < 5000; ++i) {
        // Stream 1: arm with 50 samples so the EOS marker sample is 50
        clock.samplesProduced(clock.streamEpoch(), 50);
        QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));

        // Stage and consume a flush via Voice so beginStream() actually publishes
        // the new epoch and resets counters + EndOfStreamMarker.
        clock.setPositionOffset(0ms);
        std::ignore = voice.stageFlush(0ms);
        std::ignore = voice.render(100, 0us);
        QCOMPARE(clock.streamSamplesRead(), std::uint64_t{0});

        // Stream 2: consumer reads 100 samples
        fillQueue(&pipe, hundredSamples);
        std::ignore = voice.render(100, 0us);
        QCOMPARE(clock.streamSamplesRead(), std::uint64_t{100});

        // Producer writes 10,000 samples
        clock.samplesProduced(clock.streamEpoch(), 10000);

        // Arm EOS for stream 2: EndOfStreamMarker packs (epoch, Pending, 10000) in one word,
        // so the consumer thread can never observe Pending with the old sample count of 50.
        QVERIFY(clock.boundaryEndOfStream(clock.streamEpoch()));
        QVERIFY(clock.retractEndOfStream());

        // Flush for next iteration
        clock.setPositionOffset(0ms);
        std::ignore = voice.stageFlush(0ms);
        std::ignore = voice.render(100, 0us);
    }

    stop.store(true, std::memory_order_relaxed);
    consumerThread.join();

    QCOMPARE(staleEosObserved.load(), 0);
}

void TestAudioOutput::testVolumeChangeWhilePlaying()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(8192, 0.5f));
    QTest::qWait(100);

    // Startup server-restore volume is reported via externalVolumeRestored
    // and adopted by the pipeline without emitting volumeChanged; no priming needed.
    QSignalSpy spy(output.get(), &PlaybackPipeline::volumeChanged);

    const float targetVolume = (qAbs(output->volume() - 0.4f) < 0.05f) ? 0.7f : 0.4f;
    output->setVolume(targetVolume);
    QVERIFY(qAbs(output->volume() - targetVolume) < 0.01f);
    QCOMPARE(spy.count(), 1);

    spy.clear();
    bool signalReceived = false;
    QObject::connect(output.get(), &PlaybackPipeline::mutedChanged, [&signalReceived](bool) {
        signalReceived = true;
    });
    QSignalSpy muteSpy(output.get(), &PlaybackPipeline::mutedChanged);

    // The PipeWire/PulseAudio server may report an initial mute state during
    // stream setup. Reset to a known unmuted state before testing the mute
    // signal, and wait for any pending callbacks to settle.
    output->setMuted(false);
    QTest::qWait(200);
    signalReceived = false;
    muteSpy.clear();

    output->setMuted(true);
    QTRY_VERIFY(output->muted());
    QTRY_VERIFY(signalReceived);
    QVERIFY(muteSpy.count() >= 1);

    signalReceived = false;
    muteSpy.clear();
    output->setMuted(false);
    QTRY_VERIFY(!output->muted());
    QVERIFY(muteSpy.count() >= 1);

    QVERIFY(output->close());
}

void TestAudioOutput::testStartupExternalVolumeRestored()
{
    auto output = createPlaybackPipeline();
    QVERIFY(output);

    QSignalSpy spy(output.get(), &PlaybackPipeline::volumeChanged);

    // Simulate server-restore event at connect (0.5 linear gain)
    constexpr float restoredGain = 0.5f;
    constexpr float expectedSlider = 1.0f - std::exp(-restoredGain * 4.60517018599f);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);
    output->open(44100, 2);
    auto *stream = output->primaryStream();
    QVERIFY2(stream != nullptr, "open() must have created the primary stream");
    // The externalVolumeRestored handler is a same-thread direct connection,
    // so adoption is complete when the emit returns. Deliberately no
    // processEvents() here: a real server-side volume report posted from the
    // backend thread would be dispatched and overwrite the adopted value
    // before the assertion.
    Q_EMIT stream->externalVolumeRestored(restoredGain);

    // Restored volume must be adopted into volume()
    QVERIFY(qAbs(output->volume() - expectedSlider) < 0.01f);

    // Adopting restored volume must NOT emit volumeChanged
    QCOMPARE(spy.count(), 0);

    // An app-initiated setVolume change must fire volumeChanged exactly once
    const float newVolume = (expectedSlider < 0.5f) ? 0.85f : 0.25f;
    output->setVolume(newVolume);
    QVERIFY(qAbs(output->volume() - newVolume) < 0.01f);
    QCOMPARE(spy.count(), 1);
}

void TestAudioOutput::testExternalVolumeChangePropagates()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonpipewireaudiosink"_s && backend != u"dragonpulseaudiosink"_s) {
        QSKIP("External volume propagation only applies to PipeWire and PulseAudio backends");
    }

    const QString uniqueTag = u"DragonVolumeTest_%1"_s.arg(QCoreApplication::applicationPid());
    const QString priorDisplayName = QGuiApplication::applicationDisplayName();
    QGuiApplication::setApplicationDisplayName(uniqueTag);
    auto nameGuard = qScopeGuard([&priorDisplayName]() {
        QGuiApplication::setApplicationDisplayName(priorDisplayName);
    });

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->setStreamName(uniqueTag);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(16384, 0.5f));
    QTest::qWait(300);

    const float initialVolume = output->volume();

    QSignalSpy spy(output.get(), &PlaybackPipeline::volumeChanged);

    bool changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputVolume(sinkInputIndex, 30)) {
            QVERIFY(output->close());
            QSKIP("pactl set-sink-input-volume failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeVolume(nodeId, 0.3f)) {
            QVERIFY(output->close());
            QSKIP("pw-cli set-param failed");
        }
        changed = true;
    }

    if (changed) {
        const bool propagated = QTest::qWaitFor(
            [&]() {
                return spy.count() > 0 && qAbs(output->volume() - initialVolume) > 0.05f;
            },
            5000);
        QVERIFY2(propagated, "External volume change should propagate to PlaybackPipeline::volumeChanged()");
    }

    QVERIFY(output->close());
}

void TestAudioOutput::testExternalMuteChangePropagates()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonpipewireaudiosink"_s && backend != u"dragonpulseaudiosink"_s) {
        QSKIP("External mute propagation only applies to PipeWire and PulseAudio backends");
    }

    const QString uniqueTag = u"DragonMuteTest_%1"_s.arg(QCoreApplication::applicationPid());
    const QString priorDisplayName = QGuiApplication::applicationDisplayName();
    QGuiApplication::setApplicationDisplayName(uniqueTag);
    auto nameGuard = qScopeGuard([&priorDisplayName]() {
        QGuiApplication::setApplicationDisplayName(priorDisplayName);
    });

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->setStreamName(uniqueTag);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(16384, 0.5f));
    QTest::qWait(300);

    // Ensure starting unmuted, settling any initial server state.
    output->setMuted(false);
    QTest::qWait(200);
    QVERIFY(!output->muted());

    QSignalSpy spy(output.get(), &PlaybackPipeline::mutedChanged);

    bool changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputMute(sinkInputIndex, true)) {
            QVERIFY(output->close());
            QSKIP("pactl set-sink-input-mute failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeMute(nodeId, true)) {
            QVERIFY(output->close());
            QSKIP("pw-cli set-param (mute) failed");
        }
        changed = true;
    }

    if (changed) {
        const bool propagated = QTest::qWaitFor(
            [&]() {
                return spy.count() > 0 && output->muted();
            },
            5000);
        QVERIFY2(propagated, "External mute change should propagate to PlaybackPipeline::mutedChanged()");
    }

    spy.clear();

    changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputMute(sinkInputIndex, false)) {
            QVERIFY(output->close());
            QSKIP("pactl set-sink-input-mute failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            QVERIFY(output->close());
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeMute(nodeId, false)) {
            QVERIFY(output->close());
            QSKIP("pw-cli set-param (unmute) failed");
        }
        changed = true;
    }

    if (changed) {
        const bool propagated = QTest::qWaitFor(
            [&]() {
                return spy.count() > 0 && !output->muted();
            },
            5000);
        QVERIFY2(propagated, "External unmute change should propagate to PlaybackPipeline::mutedChanged()");
    }

    QVERIFY(output->close());
}

void TestAudioOutput::testSdlPausedSeekAndCrossfadePauseCount()
{
    if (currentSinkBackend() != u"dragonsdlaudiosink"_s) {
        QSKIP("SDL paused-stream accounting test requires dragonsdlaudiosink");
    }

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipeA(65536);
    output->setAudioPipe(&pipeA);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipeA, std::vector<float>(32000, 0.5f));
    output->samplesProduced(output->streamEpoch(), 32000);
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > 0, 5000);

    // Pausing, seeking while paused (which closes the paused stream), reopening,
    // and calling pause() again (including idempotent repeat) must pause SDL.
    output->pause();
    QVERIFY(output->isPaused());
    std::ignore = output->setPositionOffset(500ms);
    QVERIFY(!output->isDeviceOpen());

    pipeA.consumer().drain();
    fillQueue(&pipeA, std::vector<float>(32000, 0.5f));
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());
    output->samplesProduced(output->streamEpoch(), 32000);

    output->pause();
    output->pause();
    QVERIFY(output->isPaused());

    QVERIFY2(waitForWrittenCounterStable(output.get()), "written counter must stay frozen after paused seek and reopen");
    const qint64 writtenWhilePaused = output->totalSamplesWritten();

    // Idempotent resume must decrement the paused count cleanly and resume playback.
    output->resume();
    output->resume();
    QVERIFY(!output->isPaused());
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > writtenWhilePaused, 5000);

    // Pausing during an active two-stream crossfade must freeze both streams,
    // and closing/resetting while paused must not leave phantom paused counts behind.
    Pipe<float> pipeB(65536);
    fillQueue(&pipeB, std::vector<float>(32000, 0.5f));
    const auto crossfadeEpoch = output->armCrossfade(&pipeA, &pipeB, 1000ms, 1000ms);
    output->open(44100, 2);
    output->samplesProduced(crossfadeEpoch, 32000);
    QVERIFY(output->isCrossfading());
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > 0, 5000);

    output->pause();
    QVERIFY(output->isPaused());
    QVERIFY2(waitForWrittenCounterStable(output.get()), "written counter must stay frozen while both crossfade streams are paused");
    QVERIFY(output->close());
    output->reset();

    Pipe<float> pipeC(65536);
    output->setAudioPipe(&pipeC);
    output->open(44100, 2);
    fillQueue(&pipeC, std::vector<float>(32000, 0.5f));
    output->samplesProduced(output->streamEpoch(), 32000);
    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isPaused());
    QVERIFY2(waitForWrittenCounterStable(output.get()), "written counter must stay frozen while the single stream is paused");
    const qint64 singleStreamPausedSamples = output->totalSamplesWritten();

    output->resume();
    QVERIFY(!output->isPaused());
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > singleStreamPausedSamples, 5000);

    QVERIFY(output->close());
}

void TestAudioOutput::testPulseAudioSecondStreamFailurePreservesFirstStream()
{
    if (currentSinkBackend() != u"dragonpulseaudiosink"_s) {
        QSKIP("PulseAudio multi-stream failure test requires dragonpulseaudiosink");
    }

    auto output = createPlaybackPipeline();
    QVERIFY(output);

    Pipe<float> pipeA(65536);
    output->setAudioPipe(&pipeA);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipeA, std::vector<float>(32000, 0.5f));
    output->samplesProduced(output->streamEpoch(), 32000);
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > 0, 5000);

    // Failing to create a second stream on the shared connection (invalid channel count)
    // must keep the PulseAudio context and active first stream intact without asserting.
    Voice dummyRenderer(&pipeA, 99, Voice::Notifiers{});
    auto failedSecond = output->sink()->createStream(44100, 99, dummyRenderer);
    QVERIFY(!failedSecond.has_value());

    const qint64 writtenBefore = output->totalSamplesWritten();
    fillQueue(&pipeA, std::vector<float>(16000, 0.5f));
    output->samplesProduced(output->streamEpoch(), 16000);
    QTRY_VERIFY_WITH_TIMEOUT(output->totalSamplesWritten() > writtenBefore, 5000);

    // Same failure via PlaybackPipeline crossfade open keeps failure local until abort.
    Pipe<float> pipeB(65536);
    std::ignore = output->armCrossfade(&pipeA, &pipeB, 500ms, 500ms);
    output->open(44100, 99);
    QVERIFY(!output->isDeviceOpen());
    QVERIFY(output->isCrossfading());

    output->abortFadingVoice();
    QVERIFY(!output->isCrossfading());
    QVERIFY(output->close());
}

QTEST_MAIN(TestAudioOutput)
#include "test_audiooutput.moc"
