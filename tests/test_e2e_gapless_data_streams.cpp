/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "helpers/fixture_generator.h"
#include "helpers/gapless_test_utils.h"
#include "test_utils.h"

#include "player/dragondiagnostics.h"
#include <DragonAudioOutput>
#include <DragonPlayer>
#include <DragonSpectrumAnalyzer>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;
using namespace Qt::StringLiterals;
using namespace GaplessTestUtils;

static constexpr int kSampleRate = 48000;
static constexpr int kDefaultChannels = 2;
static constexpr int kDurationFrames = 4 * kSampleRate; // 4 seconds
static constexpr std::chrono::milliseconds kTrackDuration{kDurationFrames * 1000 / kSampleRate};
static constexpr int kObservationSeconds = 2;
static constexpr std::chrono::milliseconds kObservationWindow{1000 * kObservationSeconds};

// FFT frames are requested at 60fps; be generous about scheduling jitter.
static constexpr int kMinFftFramesPerSecond = 20;
static constexpr int kMaxFftFramesPerSecond = 200;

// Mirrors examples/qwidget/dragonplaylist.cpp: keeps the player's next-source
// slot filled with the track after the current one.
class ExamplePlaylist
{
public:
    explicit ExamplePlaylist(DragonPlayer *player)
        : m_player(player)
    {
        QObject::connect(player, &DragonPlayer::trackChanged, player, [this]() {
            if (m_currentIndex >= 0 && m_currentIndex < int(m_tracks.size()) - 1) {
                ++m_currentIndex;
                updatePlayerQueue();
            }
        });
        QObject::connect(player, &DragonPlayer::sourceChanged, player, [this]() {
            const QUrl current = m_player->source();
            for (int i = 0; i < int(m_tracks.size()); ++i) {
                if (m_tracks[std::size_t(i)] == current) {
                    if (i >= 0 && i != m_currentIndex) {
                        m_currentIndex = i;
                        updatePlayerQueue();
                    }
                    break;
                }
            }
        });
    }

    void addTracks(const QList<QUrl> &urls)
    {
        if (urls.isEmpty()) {
            return;
        }
        const bool wasEmpty = m_tracks.empty();
        for (const QUrl &url : urls) {
            m_tracks.push_back(url);
        }
        if (wasEmpty) {
            m_currentIndex = 0;
            m_player->setSource(m_tracks.front());
            m_player->play();
        }
        updatePlayerQueue();
    }

private:
    void updatePlayerQueue()
    {
        if (m_currentIndex >= 0 && m_currentIndex < int(m_tracks.size()) - 1) {
            m_player->setNextSource(m_tracks[std::size_t(m_currentIndex + 1)]);
        } else {
            m_player->setNextSource(QUrl{});
        }
    }

    DragonPlayer *const m_player;
    std::vector<QUrl> m_tracks;
    int m_currentIndex = -1;
};

class TestGaplessDataStreams : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPositionAndFftSurviveGaplessTransition();
};

void TestGaplessDataStreams::testPositionAndFftSurviveGaplessTransition()
{
    using namespace FixtureGenerator;

    const QByteArray sinkName = qgetenv("DRAGON_AUDIO_SINK");
    if (sinkName.isEmpty() || sinkName == "dragonnullaudiosink") {
        QSKIP("No audio sink selected (set DRAGON_AUDIO_SINK for a per-backend run)");
    }

    auto fixtureA = makeEndMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureB = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    auto fixtureC = makeStartMarkerFixture(kSampleRate, kDefaultChannels, kDurationFrames);
    FixtureGuard guardA{fixtureA.filePath};
    FixtureGuard guardB{fixtureB.filePath};
    FixtureGuard guardC{fixtureC.filePath};

    QVERIFY(QFileInfo::exists(fixtureA.filePath));
    QVERIFY(QFileInfo::exists(fixtureB.filePath));
    QVERIFY(QFileInfo::exists(fixtureC.filePath));

    DragonPlayer player;
    if (player.audioOutput()->backend() == DragonAudioOutput::Backend::Null) {
        QSKIP("Selected audio sink not available on this machine");
    }
    DragonDiagnostics diagnostics(&player);
    PlayerHelper helper(&player);

    // The qwidget example's visualizers: a spectrum analyzer tapped into the
    // sink, active for the whole playback.
    DragonSpectrumAnalyzer analyzer(&player);
    analyzer.setFrameRate(60);
    analyzer.setMode(DragonSpectrumAnalyzer::Mode::Both);

    QTimer diagTimer;
    diagTimer.setInterval(50);
    QObject::connect(&diagTimer, &QTimer::timeout, &player, [&]() {
        std::optional<std::chrono::microseconds> bufUs = diagnostics.audioBufferDuration();
        Q_UNUSED(bufUs);
        const std::size_t decQ = diagnostics.decodeQueueSize();
        Q_UNUSED(decQ);
        const std::size_t fftQ = diagnostics.fftQueueSize();
        Q_UNUSED(fftQ);
        const int underr = diagnostics.audioUnderrunCount();
        Q_UNUSED(underr);
        const auto aPos = diagnostics.audioPosition();
        Q_UNUSED(aPos);
        const qint64 samplesRead = diagnostics.audioSamplesRead();
        Q_UNUSED(samplesRead);
    });
    diagTimer.start();

    QElapsedTimer testClock;
    testClock.start();
    std::vector<qint64> frameTimes;
    QObject::connect(&analyzer, &DragonSpectrumAnalyzer::frameReady, &player, [&]() {
        frameTimes.push_back(testClock.elapsed());
    });

    ExamplePlaylist playlist(&player);
    playlist.addTracks({QUrl::fromLocalFile(fixtureA.filePath), QUrl::fromLocalFile(fixtureB.filePath), QUrl::fromLocalFile(fixtureC.filePath)});

    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");
    QVERIFY2(diagnostics.isAudioActive(), "Audio should be active after play");

    QTRY_VERIFY2_WITH_TIMEOUT(!frameTimes.empty(), "FFT frames must be emitted during track 1", 15000);
    QTest::qWait(500);
    QVERIFY2(player.position() > 0ms, "Position must advance during track 1");

    // First audible boundary: track 1 -> track 2.
    QVERIFY2(helper.waitForTrackChange(20000), "Gapless track change to track 2 did not occur within timeout");
    QVERIFY2(player.source() == QUrl::fromLocalFile(fixtureB.filePath), "Player must report track 2 as current source after the transition");

    const std::chrono::milliseconds positionAtBoundary = player.position();
    QVERIFY2(positionAtBoundary < 700ms, qPrintable(u"Position must reset at the gapless boundary, got %1ms"_s.arg(positionAtBoundary.count())));

    const qint64 observationStartMs = testClock.elapsed();
    QTest::qWait(int(kObservationWindow.count()));
    const qint64 observationEndMs = testClock.elapsed();

    const std::chrono::milliseconds positionAfterObservation = player.position();
    QVERIFY2(
        positionAfterObservation > positionAtBoundary + 1s,
        qPrintable(u"Position must keep advancing after the gapless transition: was %1ms at the boundary, is %2ms %3s later"_s.arg(positionAtBoundary.count())
                       .arg(positionAfterObservation.count())
                       .arg(kObservationSeconds)));
    QVERIFY2(positionAfterObservation < kTrackDuration + 1500ms,
             qPrintable(u"Position after the transition must stay within track 2's duration (%1ms), got %2ms"_s.arg(kTrackDuration.count())
                            .arg(positionAfterObservation.count())));

    const auto framesDuringObservation = std::count_if(frameTimes.begin(), frameTimes.end(), [&](qint64 t) {
        return t >= observationStartMs && t <= observationEndMs;
    });
    const double observedFps = static_cast<double>(framesDuringObservation) / kObservationSeconds;
    qDebug() << "FFT frames: total=" << frameTimes.size() << "during observation window=" << framesDuringObservation << "fps=" << observedFps
             << "position at boundary=" << positionAtBoundary.count() << "ms"
             << "position after observation=" << positionAfterObservation.count() << "ms";
    QVERIFY2(framesDuringObservation > 0, "FFT frames must keep being emitted after the gapless transition");
    QVERIFY2(observedFps >= kMinFftFramesPerSecond,
             qPrintable(u"FFT frame rate collapsed after the transition: %1 fps (need >= %2)"_s.arg(observedFps).arg(kMinFftFramesPerSecond)));
    QVERIFY2(observedFps <= kMaxFftFramesPerSecond,
             qPrintable(u"FFT frame rate exploded after the transition: %1 fps (must stay <= %2)"_s.arg(observedFps).arg(kMaxFftFramesPerSecond)));

    // The state machine must have stayed in the gapless transition contract.
    QVERIFY2(diagnostics.isAudioActive(), "Audio must still be active after the transition");

    player.stop();
}

QTEST_MAIN(TestGaplessDataStreams)
#include "test_e2e_gapless_data_streams.moc"
