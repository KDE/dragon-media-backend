/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * End-to-end gapless transition tests using the PulseAudio audio backend,
 * with PCM captured via PipeWire monitor ports on a null sink.
 *
 * PulseAudio on PipeWire routes playback streams through the PW graph, so
 * the same PwCaptureStream used by test_e2e_pipewire_gapless can observe
 * the rendered audio regardless of which client API produced it.
 */

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "helpers/gapless_test_utils.h"
#include "helpers/pw_capture_stream.h"
#include "test_utils.h"

#include "player/dragondiagnostics.h"
#include <DragonMultimedia/dragonplayer.h>

using namespace Qt::StringLiterals;
using namespace GaplessTestUtils;

static constexpr int kSampleRate = 48000;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 144000; // ~3 seconds at 48000Hz

class TestPulseGapless : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testGaplessSameFormat();
};

void TestPulseGapless::testGaplessSameFormat()
{
    using namespace FixtureGenerator;

    ScopedEnvVar sinkEnv("DRAGONMULTIMEDIA_AUDIO_SINK", "dragonpulseaudiosink");

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));

    PwCaptureStream capture;
    if (!capture.connect(u"dragon-test-sink"_s, kSampleRate, kDefaultChannels)) {
        QSKIP(
            "dragon-test-sink not available. To enable this test:\n"
            "  1. Create ~/.config/pipewire/pipewire.conf.d/99-dragon-test-sink.conf with:\n"
            "     context.objects = [\n"
            "         { factory = adapter\n"
            "             args = {\n"
            "                 factory.name             = support.null-audio-sink\n"
            "                 node.name                = \"dragon-test-sink\"\n"
            "                 media.class              = \"Audio/Sink\"\n"
            "                 audio.rate               = 48000\n"
            "                 audio.channels           = 2\n"
            "                 audio.position           = \"FL,FR\"\n"
            "                 monitor.channel-volumes  = true\n"
            "                 adapter.auto-port-config = {\n"
            "                     mode = dsp\n"
            "                     monitor = true\n"
            "                 }\n"
            "             }\n"
            "         }\n"
            "     ]\n"
            "  2. Restart PipeWire + WirePlumber:\n"
            "     systemctl --user restart pipewire pipewire-pulse wireplumber\n"
            "  3. Make dragon-test-sink the default Pulse sink:\n"
            "     pactl set-default-sink dragon-test-sink");
    }
    qDebug() << "Capture stream connected, waiting for data flow...";

    DragonPlayer player;
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    auto waitForCapture = [&capture]() {
        return QTest::qVerify(capture.waitForData(10000), "Capture stream should receive PCM data within timeout", "", __FILE__, __LINE__);
    };
    QVERIFY(runGaplessPlayback(player, helper, diagnostics, fixtureA, fixtureB, waitForCapture));

    capture.stop();

    auto capturedPcm = capture.takePcm();
    QVERIFY(verifyGaplessPcm(capturedPcm, fixtureA, fixtureB, kDefaultChannels));
}

QTEST_MAIN(TestPulseGapless)
#include "test_e2e_pulseaudio_gapless.moc"
