/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>
#include <stdfloat>

#include "logging_timestamp_init.h"

#include "dragonpipe_test_utils.h"
#include "player/dragonpipe.h"
#include "sink/dragonaudiosink.h"
#include "sink/dragonaudiosinkfactory.h"

#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

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

    void testGaplessTransition();

    void testStartPaused();

    void testPauseResumeCycle();

    void testIsPausedBasic();
    void testIsPausedAfterStop();

    void testSeekWhilePaused();
    void testPositionStabilityDuringPause();

    void testVolumeChangeWhilePlaying();
    void testExternalVolumeChangePropagates();
    void testExternalMuteChangePropagates();

    void testDrainCallback();

private:
    void fillQueue(DragonPipe<float> *pipe, const std::vector<float> &data);
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
    auto output = createAudioSink();
    QVERIFY(output);

    QVERIFY(qAbs(output->volume() - 1.0f) < 0.01f);
    QVERIFY(!output->muted());
    QVERIFY(!output->isDeviceOpen());
    QVERIFY(output->isQueueReady());
    QCOMPARE(output->positionMs(), 0);
    QCOMPARE(output->totalSamplesWritten(), 0);
}

void TestAudioOutput::testVolumeSetGet()
{
    auto output = createAudioSink();
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
    auto output = createAudioSink();
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
    auto output = createAudioSink();
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
    auto output = createAudioSink();
    QVERIFY(output);
    QSignalSpy spy(output.get(), &DragonAudioSink::volumeChanged);

    output->setVolume(0.7f);
    QVERIFY(spy.count() > 0);

    int countBefore = spy.count();
    output->setVolume(0.7f);
    QVERIFY2(spy.count() == countBefore, "Setting same volume should not emit signal");

    QSignalSpy muteSpy(output.get(), &DragonAudioSink::mutedChanged);
    output->setMuted(true);
    QVERIFY(muteSpy.count() > 0);
}

void TestAudioOutput::testSetQueue()
{
    auto output = createAudioSink();
    QVERIFY(output);
    DragonPipe<float> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(!output->isDeviceOpen());

    output->setAudioPipe(nullptr);
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testSetStreamNameSmoke()
{
    auto output = createAudioSink();
    QVERIFY(output);

    output->setStreamName("Test Audio"_L1);
    output->setStreamName(""_L1);
    output->setStreamName("Longer Name With Spaces"_L1);
    QVERIFY2(!output->isDeviceOpen(), "setStreamName should not open the audio device");
}

void TestAudioOutput::testPositionMsCalculation()
{
    auto output = createAudioSink();
    QVERIFY(output);

    QVERIFY(output->positionMs() == 0);
    QVERIFY(output->totalSamplesWritten() == 0);

    output->setPositionOffset(1000, DragonAudioSink::PositionResetMode::NormalTrackChange);
    QVERIFY2(output->positionMs() == 1000, "positionMs should reflect the offset set via setPositionOffset");
}

void TestAudioOutput::testTotalSamplesWritten()
{
    auto output = createAudioSink();
    QVERIFY(output);

    QVERIFY(output->totalSamplesWritten() == 0);

    output->reset();
    QVERIFY(output->totalSamplesWritten() == 0);
}

void TestAudioOutput::testReset()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    output->reset();
    QVERIFY(output->positionMs() == 0);
    QVERIFY(output->totalSamplesWritten() == 0);

    output->close();
}

void TestAudioOutput::testStopWithoutStart()
{
    auto output = createAudioSink();
    QVERIFY(output);

    output->close();
    QVERIFY(!output->isDeviceOpen());

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testStartStopLifecycle()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    QTest::qWait(100);

    output->close();

    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testMultipleStartStopCycles()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 3; ++i) {
        output->open(44100, 2);
        QTest::qWait(50);
        output->close();
        output->reset();
    }

    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testAudioDataProcessing()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    std::vector<float> audioData(4096, 0.5f);
    fillQueue(&pipe, audioData);

    output->open(44100, 2);

    QTest::qWait(50);
    fillQueue(&pipe, std::vector<float>(2048, 0.3f));

    QTest::qWait(100);

    qint64 samples = output->totalSamplesWritten();
    QVERIFY2(samples > 0, qPrintable(u"Expected some samples to be processed, got %1"_s.arg(samples)));

    output->close();
}

void TestAudioOutput::testPositionTrackingWithData()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    std::vector<float> oneSecond(44100, 0.5f);
    fillQueue(&pipe, oneSecond);

    QTest::qWait(1500);

    qint64 posMs = output->positionMs();
    QVERIFY2(posMs >= 100 && posMs <= 800, qPrintable(u"Expected position in [100, 800]ms after feeding 1s of audio (44100 stereo), got %1 ms"_s.arg(posMs)));

    output->close();
    output->reset();

    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testQueueBehavior()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);

    output->setAudioPipe(&pipe);
    QVERIFY(!output->isDeviceOpen());

    auto output2 = createAudioSink();
    if (!output2)
        QSKIP("No audio sink available");

    DragonPipe<float> pipe2(65536);
    output2->setAudioPipe(&pipe2);
    output2->open(44100, 2);
    QVERIFY2(output2->isDeviceOpen(), "Second sink device should be open after open()");

    fillQueue(&pipe2, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    QVERIFY2(output2->totalSamplesWritten() > 0, "Second sink should have consumed samples from pipe");

    output2->close();
    QVERIFY2(!output2->isDeviceOpen(), "Second sink should be closed after close()");
}

void TestAudioOutput::testStartWhileAlreadyStarted()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    output->open(48000, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(48000, 2));

    output->close();
}

void TestAudioOutput::testStopWithActiveCallbacks()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(8192, 0.5f));

    QTest::qWait(20);

    fillQueue(&pipe, std::vector<float>(4096, 0.3f));

    output->close();
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testRapidStartStopCycles()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    for (int i = 0; i < 10; ++i) {
        output->open(44100, 2);
        QVERIFY(output->isDeviceOpen());

        fillQueue(&pipe, std::vector<float>(2048, 0.5f));

        QTest::qWait(5);

        fillQueue(&pipe, std::vector<float>(2048, 0.3f));

        output->close();
        QVERIFY(!output->isDeviceOpen());
        output->reset();
    }

    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testStopDuringStarvation()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    output->close();
    QVERIFY(!output->isDeviceOpen());

    output->reset();
    QVERIFY(output->positionMs() == 0);
}

void TestAudioOutput::testQueueReadyApi()
{
    auto output = createAudioSink();
    QVERIFY(output);

    QVERIFY(output->isQueueReady());

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    output->setQueueReady(true);
    QVERIFY(output->isQueueReady());

    output->setQueueReady(true);
    QVERIFY(output->isQueueReady());
    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());
    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());
}

void TestAudioOutput::testFlushOpensGate()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(50);

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    output->setPositionOffset(0, DragonAudioSink::PositionResetMode::NormalTrackChange);

    QTest::qWait(100);

    QVERIFY2(output->isQueueReady(), "SDL callback should have processed flush and opened the gate");

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testGaplessTransition()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(65536, 0.5f));
    QTest::qWait(300);

    const qint64 samplesBefore = output->totalSamplesWritten();
    QVERIFY2(samplesBefore > 10000, "Counter should have grown large after 300ms of playback");
    const size_t queueSizeBefore = pipe.consumer().ready();
    QVERIFY2(queueSizeBefore > 0, "Queue should still have items");

    output->setPositionOffset(0, DragonAudioSink::PositionResetMode::GaplessTransition);

    QTest::qWait(100);

    const size_t queueSizeAfter = pipe.consumer().ready();
    QVERIFY2(queueSizeAfter > 0, "Queue should not be drained during GaplessTransition");

    output->close();
    QVERIFY(!output->isDeviceOpen());

    const qint64 samplesAfter = output->totalSamplesWritten();
    QVERIFY2(samplesAfter < 20000, "totalSamplesWritten should be small after GaplessTransition reset");
}

void TestAudioOutput::testStartPaused()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();

    QVERIFY(output->isPaused());
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    QCOMPARE(output->positionMs(), 0);
    QCOMPARE(output->totalSamplesWritten(), 0);

    output->resume();
    QVERIFY(!output->isPaused());
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->hasFormat(44100, 2));

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testPauseResumeCycle()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    const qint64 posBefore = output->positionMs();
    const qint64 writtenBefore = output->totalSamplesWritten();

    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(output->isPaused());

    QTest::qWait(200);

    QCOMPARE(output->positionMs(), posBefore);

    const qint64 writtenAfterPause = output->totalSamplesWritten();
    QVERIFY2(writtenAfterPause <= writtenBefore + 2048, "Pause should stop or significantly reduce sample consumption");

    QTest::qWait(200);
    QCOMPARE(output->totalSamplesWritten(), writtenAfterPause);

    output->resume();
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    output->close();
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testIsPausedBasic()
{
    auto output = createAudioSink();
    QVERIFY(output);

    QVERIFY(!output->isPaused());

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());
    QVERIFY(!output->isPaused());

    output->pause();
    QVERIFY(output->isPaused());

    output->resume();
    QVERIFY(!output->isPaused());

    output->close();
}

void TestAudioOutput::testIsPausedAfterStop()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    output->pause();
    QVERIFY(output->isPaused());

    output->close();
    QVERIFY(!output->isPaused());
    QVERIFY(!output->isDeviceOpen());
}

void TestAudioOutput::testSeekWhilePaused()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
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

    output->setQueueReady(false);
    QVERIFY(!output->isQueueReady());

    const qint64 seekTargetMs = 30000;
    output->setPositionOffset(seekTargetMs, DragonAudioSink::PositionResetMode::Seek);

    const qint64 posAfterSeek = output->positionMs();
    QCOMPARE(posAfterSeek, seekTargetMs);

    QCOMPARE(pipe.consumer().ready(), size_t(0));

    QVERIFY(output->isQueueReady());

    output->resume();
    QVERIFY(!output->isPaused());

    output->close();
}

void TestAudioOutput::testPositionStabilityDuringPause()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);

    fillQueue(&pipe, std::vector<float>(4096, 0.5f));
    QTest::qWait(100);

    output->pause();
    const qint64 posAtPause = output->positionMs();

    for (int i = 0; i < 5; ++i) {
        QTest::qWait(100);
        const qint64 currentPos = output->positionMs();
        QCOMPARE(currentPos, posAtPause);
    }

    output->resume();
    QTest::qWait(100);

    const qint64 posAfterResume = output->positionMs();
    QVERIFY2(posAfterResume >= posAtPause, "Position must not go backward after resume from pause");

    output->close();
}

void TestAudioOutput::fillQueue(DragonPipe<float> *pipe, const std::vector<float> &data)
{
    pipe->producer().write(data, std::stop_token{});
}

QString TestAudioOutput::currentSinkBackend()
{
    return qEnvironmentVariable("DRAGONMULTIMEDIA_AUDIO_SINK");
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
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    QSignalSpy drainSpy(output.get(), &DragonAudioSink::drained);
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

    output->notifyDecodeFinished();

    const bool drained = QTest::qWaitFor(
        [&]() {
            return drainSpy.count() > 0;
        },
        5000);
    QVERIFY2(drained, "drained() signal should fire after decode finished and pipe empty");

    output->close();
}

void TestAudioOutput::testVolumeChangeWhilePlaying()
{
    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(8192, 0.5f));
    QTest::qWait(100);

    QSignalSpy spy(output.get(), &DragonAudioSink::volumeChanged);
    spy.clear();

    output->setVolume(0.4f);
    QVERIFY(qAbs(output->volume() - 0.4f) < 0.01f);
    QVERIFY(spy.count() >= 1);

    spy.clear();
    bool signalReceived = false;
    QObject::connect(output.get(), &DragonAudioSink::mutedChanged, [&signalReceived](bool) {
        signalReceived = true;
    });
    QSignalSpy muteSpy(output.get(), &DragonAudioSink::mutedChanged);

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

    output->close();
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

    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
    output->setAudioPipe(&pipe);

    output->setStreamName(uniqueTag);
    output->open(44100, 2);
    QVERIFY(output->isDeviceOpen());

    fillQueue(&pipe, std::vector<float>(16384, 0.5f));
    QTest::qWait(300);

    const float initialVolume = output->volume();

    QSignalSpy spy(output.get(), &DragonAudioSink::volumeChanged);

    bool changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            output->close();
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputVolume(sinkInputIndex, 30)) {
            output->close();
            QSKIP("pactl set-sink-input-volume failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            output->close();
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeVolume(nodeId, 0.3f)) {
            output->close();
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
        QVERIFY2(propagated, "External volume change should propagate to DragonAudioSink::volumeChanged()");
    }

    output->close();
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

    auto output = createAudioSink();
    QVERIFY(output);

    DragonPipe<float> pipe(65536);
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

    QSignalSpy spy(output.get(), &DragonAudioSink::mutedChanged);

    bool changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            output->close();
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputMute(sinkInputIndex, true)) {
            output->close();
            QSKIP("pactl set-sink-input-mute failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            output->close();
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeMute(nodeId, true)) {
            output->close();
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
        QVERIFY2(propagated, "External mute change should propagate to DragonAudioSink::mutedChanged()");
    }

    spy.clear();

    changed = false;
    if (backend == u"dragonpulseaudiosink"_s) {
        uint32_t sinkInputIndex = 0;
        if (!findSinkInputByApplicationName(uniqueTag, sinkInputIndex)) {
            output->close();
            QSKIP("Could not locate PulseAudio sink-input for test stream");
        }
        if (!setSinkInputMute(sinkInputIndex, false)) {
            output->close();
            QSKIP("pactl set-sink-input-mute failed");
        }
        changed = true;
    } else {
        uint32_t nodeId = 0;
        if (!findPwNodeByApplicationName(uniqueTag, nodeId)) {
            output->close();
            QSKIP("Could not locate PipeWire node for test stream");
        }
        if (!setPwNodeMute(nodeId, false)) {
            output->close();
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
        QVERIFY2(propagated, "External unmute change should propagate to DragonAudioSink::mutedChanged()");
    }

    output->close();
}

QTEST_MAIN(TestAudioOutput)
#include "test_audiooutput.moc"
