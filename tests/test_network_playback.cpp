/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Network playback test - serves a local WMA fixture via HTTP and plays it.
 * Self-contained test that doesn't depend on external websites.
 */

#include <QtCore>
#include <QtNetwork>
#include <QtTest>

#include "logging_timestamp_init.h"

#include "player/dragondiagnostics.h"
#include "test_utils.h"
#include <DragonPlayer>

#include "testhttpserver.h"

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

class TestNetworkPlayback : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void cleanupTestCase();

    void testPlayLocalWmaFileOverHttp();
    void testRadioToLocalFileTransition();
    void testSeekHttpFile();
    void testStatusOrderingDuringSlowLoad();
    void testSeekableDependsOnServerCapability();

private:
    TestHttpServer *m_server = nullptr;
    QString fixturePath(const QString &filename);
};

QString TestNetworkPlayback::fixturePath(const QString &filename)
{
    QString fixturesDir = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR);
    return fixturesDir + u"/"_s + filename;
}

void TestNetworkPlayback::initTestCase()
{
    m_server = new TestHttpServer(this);
    QVERIFY2(m_server->start(), "Failed to start HTTP server");
}

void TestNetworkPlayback::init()
{
    m_server->clearThrottle();
}

void TestNetworkPlayback::cleanupTestCase()
{
    if (m_server) {
        m_server->stop();
        delete m_server;
        m_server = nullptr;
    }
}

void TestNetworkPlayback::testPlayLocalWmaFileOverHttp()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    qDebug() << "Testing playback from:" << url.toString();

    DragonPlayer player;

    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);
    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);
    QSignalSpy durationSpy(&player, &DragonPlayer::durationChanged);

    player.setSource(url);

    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 5000);
    QVERIFY(player.source() == url);

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto s = player.status();
            return s == DragonPlayer::MediaStatus::LoadedMedia || s == DragonPlayer::MediaStatus::BufferedMedia || s == DragonPlayer::MediaStatus::InvalidMedia
                || errorSpy.count() > 0;
        }(),
        10000);

    if (errorSpy.count() > 0) {
        auto error = errorSpy.last().at(0).value<DragonPlayer::Error>();
        QFAIL(qPrintable(u"Error loading media: %1"_s.arg(static_cast<int>(error))));
    }

    auto status = player.status();
    qDebug() << "Media status after load:" << static_cast<int>(status);

    if (status != DragonPlayer::MediaStatus::LoadedMedia && status != DragonPlayer::MediaStatus::BufferedMedia) {
        QSKIP("Media could not be loaded - skipping test");
    }

    QTRY_VERIFY_WITH_TIMEOUT(durationSpy.count() > 0 || player.duration().value_or(0ms) > 0ms, 5000);
    qDebug() << "Duration:" << player.duration().value_or(0ms).count() << "ms";
    QVERIFY(player.duration().value_or(0ms) > 0ms);

    qDebug() << "Starting playback...";
    player.play();

    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    DragonDiagnostics diag(&player);
    QVERIFY2(diag.isAudioActive(), "Audio should be active during playback");
    qDebug() << "Playback started! Position:" << player.position() << "ms";

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return player.playbackState() == DragonPlayer::PlaybackState::StoppedState || player.status() == DragonPlayer::MediaStatus::EndOfMedia;
        }(),
        60000);

    qDebug() << "Playback completed!";
    qDebug() << "Final status:" << static_cast<int>(player.status());
    qDebug() << "Final state:" << static_cast<int>(player.playbackState());

    QVERIFY2(player.status() == DragonPlayer::MediaStatus::EndOfMedia, "Status should be EndOfMedia");
    QVERIFY2(player.playbackState() == DragonPlayer::PlaybackState::StoppedState, "Playback state should be Stopped");
    QVERIFY2(!diag.isAudioActive(), "Audio should be inactive");

    qDebug() << "Network playback test completed successfully!";
}

void TestNetworkPlayback::testRadioToLocalFileTransition()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    m_server->serveFile(wmaPath);

    QUrl networkUrl;
    networkUrl.setScheme(u"http"_s);
    networkUrl.setHost(u"localhost"_s);
    networkUrl.setPort(m_server->port());
    networkUrl.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    qDebug() << "Testing radio stream from:" << networkUrl.toString();

    DragonPlayer player;

    QSignalSpy bufferProgressSpy(&player, &DragonPlayer::bufferProgressChanged);
    QSignalSpy sourceSpy(&player, &DragonPlayer::sourceChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(networkUrl);

    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 5000);
    QVERIFY(player.source() == networkUrl);

    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return player.bufferProgress() > 0.0 || player.status() == DragonPlayer::MediaStatus::LoadedMedia;
        }(),
        10000);

    qreal progressBeforeSwitch = player.bufferProgress();
    qDebug() << "Buffer progress before switch:" << progressBeforeSwitch;
    QVERIFY2(progressBeforeSwitch > 0.0 || player.status() == DragonPlayer::MediaStatus::LoadedMedia,
             "Buffer progress should be positive or media should be loaded for network stream");

    QString localPath = fixturePath(u"sample-3s.mp3"_s);
    QVERIFY2(QFile::exists(localPath), qPrintable(u"MP3 fixture not found: %1"_s.arg(localPath)));

    QUrl localUrl = QUrl::fromLocalFile(localPath);
    qDebug() << "Switching to local file:" << localUrl.toString();

    bufferProgressSpy.clear();
    sourceSpy.clear();

    player.setSource(localUrl);

    QTRY_VERIFY_WITH_TIMEOUT(sourceSpy.count() > 0, 5000);
    QVERIFY(player.source() == localUrl);

    qreal progressAfterSwitch = player.bufferProgress();
    qDebug() << "Buffer progress immediately after switch:" << progressAfterSwitch;

    QTest::qWait(100);

    progressAfterSwitch = player.bufferProgress();
    qDebug() << "Buffer progress after switch + 100ms:" << progressAfterSwitch;

    QVERIFY2(bufferProgressSpy.count() > 0, "Should have received buffer progress signal after switching to local file");

    QVERIFY2(player.status() == DragonPlayer::MediaStatus::LoadedMedia || player.status() == DragonPlayer::MediaStatus::LoadingMedia
                 || player.status() == DragonPlayer::MediaStatus::BufferedMedia,
             "Local file should be in loading or loaded state");

    if (player.status() == DragonPlayer::MediaStatus::LoadedMedia || player.status() == DragonPlayer::MediaStatus::BufferedMedia) {
        QCOMPARE(player.bufferProgress(), 1.0);
    }

    qDebug() << "Radio to local file transition test completed!";
    qDebug() << "  Final bufferProgress:" << player.bufferProgress();
    qDebug() << "  Final status:" << static_cast<int>(player.status());
}

void TestNetworkPlayback::testSeekHttpFile()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    m_server->serveFile(wmaPath);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    qDebug() << "Testing seek with HTTP file:" << url.toString();

    DragonPlayer player;
    QSignalSpy positionSpy(&player, &DragonPlayer::positionChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(url);

    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia || player.status() == DragonPlayer::MediaStatus::BufferedMedia, 10000);

    QVERIFY2(player.seekable(), "HTTP file should be seekable");
    QTRY_VERIFY2_WITH_TIMEOUT(player.duration().value_or(0ms) > 0ms, "Duration should be known", 5000);

    player.play();
    QTRY_VERIFY_WITH_TIMEOUT(player.playbackState() == DragonPlayer::PlaybackState::PlayingState, 5000);

    QTRY_VERIFY_WITH_TIMEOUT(player.position() > 500ms, 5000);

    positionSpy.clear();
    qDebug() << "Seeking to 1000ms";
    player.setPosition(1000ms);

    QTRY_VERIFY_WITH_TIMEOUT(positionSpy.count() > 0, 5000);

    qDebug() << "Position after seek:" << player.position();
    QVERIFY2(player.position() >= 900ms && player.position() <= 1200ms, "Position should be near 1000ms after seek");

    QTest::qWait(500);
    QVERIFY2(player.position() > 1000ms, "Playback should have advanced past 1000ms");

    player.stop();
    qDebug() << "Seek HTTP file test completed successfully!";
}

void TestNetworkPlayback::testStatusOrderingDuringSlowLoad()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    m_server->serveFile(wmaPath);
    m_server->setThrottle(4 * 1024, 20);

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonPlayer player;
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);
    QVERIFY(statusSpy.isValid());

    player.setSource(url);

    QTRY_VERIFY_WITH_TIMEOUT(player.duration().value_or(0ms) > 0ms, 15000);

    int loadedIndex = -1;
    for (int i = 0; i < statusSpy.count(); ++i) {
        const auto status = statusSpy.at(i).at(0).value<DragonPlayer::MediaStatus>();
        if (status == DragonPlayer::MediaStatus::LoadedMedia) {
            loadedIndex = i;
            break;
        }
        QVERIFY2(status != DragonPlayer::MediaStatus::StalledMedia && status != DragonPlayer::MediaStatus::BufferingMedia
                     && status != DragonPlayer::MediaStatus::BufferedMedia,
                 qPrintable(u"Status jumped to %1 before LoadedMedia"_s.arg(static_cast<int>(status))));
    }
    QVERIFY2(loadedIndex >= 0, "Media never reached LoadedMedia");

    player.stop();
}

void TestNetworkPlayback::testSeekableDependsOnServerCapability()
{
    QString wmaPath = fixturePath(u"gs-16b-1c-44100hz.wma"_s);
    QVERIFY2(QFile::exists(wmaPath), qPrintable(u"WMA fixture not found: %1"_s.arg(wmaPath)));

    QUrl url;
    url.setScheme(u"http"_s);
    url.setHost(u"localhost"_s);
    url.setPort(m_server->port());
    url.setPath(u"/gs-16b-1c-44100hz.wma"_s);

    DragonPlayer player;
    QSignalSpy seekableSpy(&player, &DragonPlayer::seekableChanged);
    QVERIFY(seekableSpy.isValid());

    m_server->setAcceptRanges(true);
    m_server->serveFile(wmaPath);

    player.setSource(url);
    QTRY_VERIFY_WITH_TIMEOUT(player.seekable(), 10000);
    QVERIFY(seekableSpy.contains(QList<QVariant>{true}));

    m_server->setAcceptRanges(false);
    seekableSpy.clear();

    QUrl url2 = url;
    url2.setQuery(u"reload=1"_s);
    player.setSource(url2);
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia || player.status() == DragonPlayer::MediaStatus::BufferedMedia, 10000);

    QVERIFY(!QTest::qWaitFor(
        [&]() {
            return player.seekable() || seekableSpy.contains(QList<QVariant>{true});
        },
        500));
    QVERIFY2(!player.seekable(), "seekable() must stay false when the server does not advertise Accept-Ranges");
    QVERIFY(!seekableSpy.contains(QList<QVariant>{true}));

    m_server->setAcceptRanges(true);
}

QTEST_MAIN(TestNetworkPlayback)
#include "test_network_playback.moc"
