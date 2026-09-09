/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests verifying that DragonPlayer::setSource() matches QMediaPlayer's
 * signal ordering: the implicit stop and LoadingMedia transitions must
 * precede sourceChanged, and state must not be force-emitted when
 * already StoppedState.
 */

#include <QtCore>
#include <QtTest>

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMediaBackend/dragonplayer.h>

#include <QSignalSpy>
#include <QUrl>

using namespace Qt::StringLiterals;

class TestPlayerSetSource : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSetSourceFromFreshPlayerNoForceEmit();
    void testSetSourceSignalOrderFromFreshPlayer();
    void testSetSourceSignalOrderFromPlaying();
    void testSetSourceSameUrlWhileStoppedReloads();

private:
    void skipIfMissing(const QString &filename)
    {
        if (!QFileInfo::exists(TestFixture::fixturePath(filename))) {
            QSKIP(qPrintable(u"Fixture not available: %1"_s.arg(filename)));
        }
    }

    void skipIfMissing(const QStringList &filenames)
    {
        for (const auto &f : filenames) {
            if (!QFileInfo::exists(TestFixture::fixturePath(f))) {
                QSKIP(qPrintable(u"Fixture not available: %1"_s.arg(f)));
            }
        }
    }
};

void TestPlayerSetSource::testSetSourceFromFreshPlayerNoForceEmit()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::NoMedia);

    QSignalSpy stateSpy(&player, &DragonPlayer::stateChanged);

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QCOMPARE(stateSpy.count(), 0);
}

void TestPlayerSetSource::testSetSourceSignalOrderFromFreshPlayer()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    SignalOrderTracker tracker(&player);
    tracker.trackSourceChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource() must emit sourceChanged");
    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s), "setSource() must emit LoadingMedia");

    int loadingIdx = tracker.indexOfPrefix(u"statusChanged(LoadingMedia)"_s);
    int sourceIdx = tracker.events().indexOf(u"sourceChanged()"_s);

    QVERIFY2(loadingIdx < sourceIdx, "statusChanged(LoadingMedia) must precede sourceChanged (QM order)");

    QVERIFY2(!tracker.containsPrefix(u"stateChanged(StoppedState"_s), "setSource from StoppedState must NOT force-emit stateChanged(StoppedState)");
}

void TestPlayerSetSource::testSetSourceSignalOrderFromPlaying()
{
    skipIfMissing({u"sample-3s.mp3"_s, u"gs-3s-2c-44100hz.ogg"_s});

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    SignalOrderTracker tracker(&player);
    tracker.trackSourceChanges();
    tracker.trackStateChanges();
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"gs-3s-2c-44100hz.ogg"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.containsTransition(DragonPlayer::PlaybackState::StoppedState, DragonPlayer::PlaybackState::PlayingState),
             "setSource while playing must emit stateChanged(StoppedState, PlayingState)");
    QVERIFY2(tracker.contains(u"statusChanged(LoadedMedia)"_s), "setSource while playing must emit LoadedMedia from implicit stop");
    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s), "setSource must emit LoadingMedia for the new source");
    QVERIFY2(tracker.contains(u"sourceChanged()"_s), "setSource must emit sourceChanged");

    QVERIFY2(tracker.verifyOrder(u"stateChanged(StoppedState, PlayingState)"_s, u"sourceChanged()"_s),
             "stateChanged(StoppedState, PlayingState) must precede sourceChanged");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"statusChanged(LoadedMedia)"_s),
             "source swap while playing must go LoadingMedia -> LoadedMedia from the real load (no synthetic LoadedMedia from the implicit stop)");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"sourceChanged()"_s), "statusChanged(LoadingMedia) must precede sourceChanged");
}

void TestPlayerSetSource::testSetSourceSameUrlWhileStoppedReloads()
{
    skipIfMissing(u"sample-3s.mp3"_s);

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.stopAndWait());
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::LoadedMedia);

    SignalOrderTracker tracker(&player);
    tracker.trackStatusChanges();

    player.setSource(QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s)));
    QTRY_VERIFY_WITH_TIMEOUT(player.status() == DragonPlayer::MediaStatus::LoadedMedia, 10000);

    QVERIFY2(tracker.contains(u"statusChanged(LoadingMedia)"_s),
             "setSource(sameUrl) while stopped with loaded media must run a real reload cycle (LoadingMedia)");
    QVERIFY2(tracker.verifyOrder(u"statusChanged(LoadingMedia)"_s, u"statusChanged(LoadedMedia)"_s),
             "reload must reach a fresh LoadedMedia after LoadingMedia so resume hooks can fire");
}

QTEST_MAIN(TestPlayerSetSource)
#include "test_player_setsource.moc"
