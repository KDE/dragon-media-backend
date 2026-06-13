/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * End-to-end gapless transition tests using an isolated PipeWire daemon
 * with null-audio-sink, boundary-marker fixtures, and PCM capture.
 *
 * Requires: pipewire binary in PATH, PipeWire libraries, support.null-audio-sink
 * Skip if any dependency is missing.
 */

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "helpers/marker_detector.h"
#include "helpers/pw_capture_stream.h"
#include "helpers/pw_isolated_daemon.h"
#include "test_utils.h"

#include <DragonMultimedia/dragondiagnostics.h>
#include <DragonMultimedia/dragonplayer.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

static constexpr int kSampleRate = 48000;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 25104;
static constexpr int kQuantumFrames = 1024;
static constexpr int kDrainMs = 100;

class TestPipeWireGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testGaplessSameFormat();
    void testGaplessFormatChange();

private:
    void runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapFrames);

    PwIsolatedDaemon *m_daemon = nullptr;
    QString m_oldPipeWireRemote;
};

void TestPipeWireGapless::initTestCase()
{
    const QString pipewirePath = QStandardPaths::findExecutable(u"pipewire"_s);
    if (pipewirePath.isEmpty()) {
        QSKIP("pipewire binary not found in PATH skipping PipeWire gapless tests");
    }

    const bool hasNullSink = []() {
        QTemporaryDir dir;
        if (!dir.isValid())
            return false;
        QString configPath = dir.filePath(u"check.conf"_s);
        QFile cfg(configPath);
        if (!cfg.open(QIODevice::WriteOnly))
            return false;
        const QByteArray config =
            "\n"
            "context.properties = { support.dbus = false mem.allow-mlock = false core.daemon = true core.name = pipewire-0 }\n"
            "context.spa-libs = { support.* = support/libspa-support audio.convert.* = audioconvert/libspa-audioconvert }\n"
            "context.modules = [\n"
            "    { name = libpipewire-module-protocol-native }\n"
            "    { name = libpipewire-module-client-node }\n"
            "    { name = libpipewire-module-adapter }\n"
            "    { name = libpipewire-module-spa-node-factory }\n"
            "    { name = libpipewire-module-spa-device-factory }\n"
            "]\n"
            "context.objects = [\n"
            "    { factory = spa-node-factory\n"
            "        args = {\n"
            "            factory.name     = support.null-audio-sink\n"
            "            node.name        = \"probe-null-sink\"\n"
            "            media.class      = Audio/Sink\n"
            "            object.linger    = false\n"
            "            audio.position   = [ FL FR ]\n"
            "        }\n"
            "    }\n"
            "]\n";
        cfg.write(config);
        cfg.close();

        QProcess proc;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"PIPEWIRE_CONFIG_DIR"_s, dir.path());
        env.insert(u"PIPEWIRE_RUNTIME_DIR"_s, dir.path());
        env.insert(u"SPA_PLUGIN_DIR"_s, u"/usr/lib64/spa-0.2"_s);
        env.insert(u"PIPEWIRE_MODULE_DIR"_s, u"/usr/lib64/pipewire-0.3"_s);
        proc.setProcessEnvironment(env);
        proc.start(u"pipewire"_s, {u"-c"_s, configPath});
        if (!proc.waitForStarted(5000))
            return false;

        QThread::msleep(500);
        bool ok = (proc.state() == QProcess::Running);
        proc.terminate();
        proc.waitForFinished(3000);
        return ok;
    }();

    if (!hasNullSink) {
        QSKIP("support.null-audio-sink not available skipping PipeWire gapless tests");
    }

    m_oldPipeWireRemote = qEnvironmentVariable("PIPEWIRE_REMOTE");
    qputenv("DRAGONMULTIMEDIA_AUDIO_SINK", "dragonpipewireaudiosink");
}

void TestPipeWireGapless::cleanupTestCase()
{
    if (m_daemon) {
        m_daemon->stop();
        delete m_daemon;
        m_daemon = nullptr;
    }
    if (m_oldPipeWireRemote.isEmpty()) {
        qunsetenv("PIPEWIRE_REMOTE");
    } else {
        qputenv("PIPEWIRE_REMOTE", m_oldPipeWireRemote.toUtf8());
    }
    qunsetenv("DRAGON_PW_TEST_SINK_NAME");
}

void TestPipeWireGapless::runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapFrames)
{
    const int channels = fixtureA.channels;

    m_daemon = new PwIsolatedDaemon();
    QVERIFY2(m_daemon->start(), "Failed to start isolated PipeWire daemon");

    qputenv("PIPEWIRE_REMOTE", m_daemon->socketPath().toUtf8());
    qputenv("DRAGON_PW_TEST_SINK_NAME", "test-null-sink");

    QTest::qWait(500);

    PwCaptureStream capture;
    QVERIFY2(capture.connect(u"test-null-sink"_s, kSampleRate, channels), "Failed to connect capture stream");

    QTest::qWait(200);

    DragonPlayer player;
    PlayerHelper helper(&player);
    auto trackSpy = SignalSpyHelper::trackSpy(&player);
    auto stateSpy = SignalSpyHelper::stateSpy(&player);

    QVERIFY(helper.setSourceAndWait(QUrl::fromLocalFile(fixtureA.filePath)));
    player.setNextSource(QUrl::fromLocalFile(fixtureB.filePath));

    QVERIFY(helper.playAndWait());
    VERIFY_AUDIO_ACTIVE(player);

    QVERIFY(helper.waitForTrackChange(30000));

    QVERIFY2(helper.verifyNoStopState(stateSpy), "Playback state should never stop during gapless transition");

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, 15000);

    QTest::qWait(kDrainMs);

    capture.stop();

    auto pcm = capture.takePcm();
    QVERIFY2(pcm.size() > 0, "Captured PCM must not be empty");

    MarkerHit aEnd = findEndMarker(pcm, channels);
    MarkerHit bStart = findStartMarker(pcm, channels);

    if (!aEnd.found || !bStart.found) {
        qWarning() << "CAPTURE DIAGNOSTICS";
        qWarning() << "===================";
        qWarning() << "Total captured frames:" << capture.totalFrames();
        qWarning() << "Track A end marker:" << (aEnd.found ? "found" : "NOT FOUND") << "frame:" << aEnd.frameIndex << "amplitude:" << aEnd.amplitude;
        qWarning() << "Track B start marker:" << (bStart.found ? "found" : "NOT FOUND") << "frame:" << bStart.frameIndex << "amplitude:" << bStart.amplitude;

        if (pcm.size() >= 40) {
            QStringList gapSamples;
            int64_t searchStart = aEnd.found ? ((aEnd.frameIndex + 1) * channels) : 0;
            for (int i = 0; i < 20 && (searchStart + i) < static_cast<int64_t>(pcm.size()); ++i) {
                gapSamples.append(QString::number(static_cast<double>(pcm[searchStart + i]), 'f', 6));
            }
            qWarning() << "First 20 samples after end marker:" << gapSamples.join(u", "_s);
        }
    }

    QVERIFY2(aEnd.found, "Track A end marker (+1.0 left channel) not found in captured PCM");
    QVERIFY2(bStart.found, "Track B start marker (-1.0 left channel) not found in captured PCM");

    int64_t gap = gapFrames(aEnd, bStart);
    QVERIFY2(gap <= expectedMaxGapFrames,
             qPrintable(u"Marker gap: %1 frames (%2 ms) expected ≤ %3 frames"_s.arg(gap).arg(gap * 1000 / kSampleRate).arg(expectedMaxGapFrames)));

    QVERIFY2(bStart.frameIndex > aEnd.frameIndex, "Track B start marker must appear after Track A end marker");

    player.stop();

    m_daemon->stop();
    delete m_daemon;
    m_daemon = nullptr;
}

void TestPipeWireGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    runGaplessScenario(fixtureA, fixtureB, 0);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

void TestPipeWireGapless::testGaplessFormatChange()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, 1, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    runGaplessScenario(fixtureA, fixtureB, kQuantumFrames);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

QTEST_MAIN(TestPipeWireGapless)
#include "test_e2e_pipewire_gapless.moc"