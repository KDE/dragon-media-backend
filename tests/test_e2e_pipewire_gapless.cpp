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

#include <cmath>
#include <vector>

using namespace Qt::StringLiterals;

static constexpr int kSampleRate = 48000;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 25104;
static constexpr int kQuantumFrames = 1024;
static constexpr int kDrainMs = 100;
static constexpr float kPcmTolerance = 1e-4f;

static bool
comparePcmRegions(const std::vector<float> &captured, const std::vector<float> &expected, int channels, float tolerance, QString *errorMsg = nullptr)
{
    if (captured.size() < expected.size()) {
        if (errorMsg) {
            *errorMsg = QStringLiteral("Captured PCM too short: %1 samples vs expected %2").arg(captured.size()).arg(expected.size());
        }
        return false;
    }

    const int extra = static_cast<int>(captured.size() - expected.size());
    const int maxShift = channels * 16;
    if (extra > maxShift) {
        if (errorMsg) {
            *errorMsg =
                QStringLiteral("Captured PCM too long: %1 samples vs expected %2 (max extra %3)").arg(captured.size()).arg(expected.size()).arg(maxShift);
        }
        return false;
    }

    for (int shift = 0; shift <= extra; shift += channels) {
        bool match = true;
        for (size_t i = 0; i < expected.size(); ++i) {
            if (std::abs(captured[shift + i] - expected[i]) > tolerance) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }

    for (size_t i = 0; i < expected.size(); ++i) {
        float diff = std::abs(captured[i] - expected[i]);
        if (diff > tolerance) {
            if (errorMsg) {
                *errorMsg = QStringLiteral("Sample %1 mismatch: captured=%2 expected=%3 diff=%4")
                                .arg(i)
                                .arg(static_cast<double>(captured[i]))
                                .arg(static_cast<double>(expected[i]))
                                .arg(static_cast<double>(diff));
            }
            return false;
        }
    }
    return true;
}

static void trimSilence(std::vector<float> &pcm, int channels, float threshold = 1e-6f)
{
    int start = 0;
    while (start < static_cast<int>(pcm.size())) {
        bool silent = true;
        for (int ch = 0; ch < channels; ++ch) {
            if (std::abs(pcm[start + ch]) > threshold) {
                silent = false;
                break;
            }
        }
        if (!silent) {
            break;
        }
        start += channels;
    }
    if (start > 0) {
        pcm.erase(pcm.begin(), pcm.begin() + start);
    }

    int end = static_cast<int>(pcm.size()) - channels;
    while (end >= 0) {
        bool silent = true;
        for (int ch = 0; ch < channels; ++ch) {
            if (std::abs(pcm[end + ch]) > threshold) {
                silent = false;
                break;
            }
        }
        if (!silent) {
            break;
        }
        end -= channels;
    }
    if (end + channels < static_cast<int>(pcm.size())) {
        pcm.erase(pcm.begin() + end + channels, pcm.end());
    }
}

class TestPipeWireGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    void testGaplessSameFormat();
    void testGaplessFormatChange();

private:
    void runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapFrames, bool comparePcm);

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
            "context.properties = { support.dbus = false mem.allow-mlock = false core.daemon = false core.name = pipewire-0 }\n"
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
        proc.setStandardOutputFile(QProcess::nullDevice());
        proc.setStandardErrorFile(QProcess::nullDevice());
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"PIPEWIRE_CONFIG_DIR"_s, dir.path());
        env.insert(u"PIPEWIRE_RUNTIME_DIR"_s, dir.path());
        QString spaDir = findSpaPluginDir();
        QString pwDir = findPipeWireModuleDir();
        if (!spaDir.isEmpty()) {
            env.insert(u"SPA_PLUGIN_DIR"_s, spaDir);
        }
        if (!pwDir.isEmpty()) {
            env.insert(u"PIPEWIRE_MODULE_DIR"_s, pwDir);
        }
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
    if (m_oldPipeWireRemote.isEmpty()) {
        qunsetenv("PIPEWIRE_REMOTE");
    } else {
        qputenv("PIPEWIRE_REMOTE", m_oldPipeWireRemote.toUtf8());
    }
    qunsetenv("DRAGON_PW_TEST_SINK_NAME");
}

void TestPipeWireGapless::runGaplessScenario(const BoundaryFixture &fixtureA, const BoundaryFixture &fixtureB, int64_t expectedMaxGapFrames, bool comparePcm)
{
    const int channels = fixtureA.channels;

    PwIsolatedDaemon daemon;
    QVERIFY2(daemon.start(), "Failed to start isolated PipeWire daemon");

    qputenv("PIPEWIRE_REMOTE", daemon.socketPath().toUtf8());
    qputenv("DRAGON_PW_TEST_SINK_NAME", "test-null-sink");

    auto daemonGuard = qScopeGuard([&] {
        daemon.stop();
        qunsetenv("PIPEWIRE_REMOTE");
        qunsetenv("DRAGON_PW_TEST_SINK_NAME");
    });

    QTest::qWait(500);

    PwCaptureStream capture;
    QVERIFY2(capture.connect(u"test-null-sink"_s, kSampleRate, channels), "Failed to connect capture stream");

    QTest::qWait(200);

    DragonPlayer player;
    PlayerHelper helper(&player);
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

    QVERIFY2(aEnd.found, "Track A end signature not found in captured PCM");
    QVERIFY2(bStart.found, "Track B start signature not found in captured PCM");

    int64_t gap = gapFrames(aEnd, bStart);
    QVERIFY2(gap <= expectedMaxGapFrames,
             qPrintable(u"Marker gap: %1 frames (%2 ms) expected ≤ %3 frames"_s.arg(gap).arg(gap * 1000 / kSampleRate).arg(expectedMaxGapFrames)));

    QVERIFY2(bStart.frameIndex > aEnd.frameIndex, "Track B start marker must appear after Track A end marker");

    if (comparePcm) {
        const int sigLen = static_cast<int>(FixtureGenerator::kEndSignature.size());

        int64_t trackAEndSample = (aEnd.frameIndex + sigLen) * channels;
        auto capturedA = std::vector<float>(pcm.begin(), pcm.begin() + trackAEndSample);

        int64_t trackBStartSample = bStart.frameIndex * channels;
        auto capturedB = std::vector<float>(pcm.begin() + trackBStartSample, pcm.end());

        trimSilence(capturedA, channels);
        trimSilence(capturedB, channels);

        QString errorMsg;
        QVERIFY2(comparePcmRegions(capturedA, fixtureA.expectedSamples, channels, kPcmTolerance, &errorMsg),
                 qPrintable(QStringLiteral("Track A PCM mismatch: %1").arg(errorMsg)));
        QVERIFY2(comparePcmRegions(capturedB, fixtureB.expectedSamples, channels, kPcmTolerance, &errorMsg),
                 qPrintable(QStringLiteral("Track B PCM mismatch: %1").arg(errorMsg)));
    }

    player.stop();
}

void TestPipeWireGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    runGaplessScenario(fixtureA, fixtureB, 0, true);

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

    runGaplessScenario(fixtureA, fixtureB, kQuantumFrames, false);

    QFile::remove(fixtureA.filePath);
    QFile::remove(fixtureB.filePath);
}

QTEST_MAIN(TestPipeWireGapless)
#include "test_e2e_pipewire_gapless.moc"