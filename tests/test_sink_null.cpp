/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include <QTemporaryDir>

#include "test_utils.h"

#include "player/pipe.h"
#include "sink/nullaudiosink.h"
#include "sink/playbackpipeline.h"
#include "sink/playbackpipelinefactory.h"
#include <DragonAudioOutput>
#include <DragonPlayer>

using namespace Qt::StringLiterals;

// The null device's error message goes through i18n(); force the C locale so
// the English substring assertions below are stable regardless of the
// environment the test suite runs in.
[[maybe_unused]] static const bool s_cLocaleForced = [] {
    qputenv("LC_ALL", "C");
    return true;
}();

class TestSinkNull : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFactoryExplicitNull();
    void testFactoryEnvRequestsNull();
    void testFactoryFallbackNoPluginsFound();
    void testNullSinkMethods();
    void testNullSinkSignals();
    void testPlayerWithNullSinkState();
};

void TestSinkNull::testFactoryExplicitNull()
{
    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::Null, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<NullAudioSink *>(pipeline->sink()) != nullptr);
}

void TestSinkNull::testFactoryEnvRequestsNull()
{
    EnvGuard guard({{"DRAGON_AUDIO_SINK", QByteArray("dragonnullaudiosink")}});

    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
}

void TestSinkNull::testFactoryFallbackNoPluginsFound()
{
    // Simulate a broken install where no audio-device plugins can be found at
    // all: KPluginMetaData::findPlugins() always checks the application
    // directory, so from within the test binary the real plugins are always
    // visible. Pointing DRAGON_AUDIO_SINK_PLUGIN_DIR at an absolute path to
    // an empty directory makes findPlugins() look only there, i.e. exactly
    // the "no plugins found" situation the null device exists for.
    QTemporaryDir emptyPluginDir;
    QVERIFY(emptyPluginDir.isValid());
    QVERIFY(QDir(emptyPluginDir.path()).mkpath(u"DragonMediaBackend/AudioSink"_s));

    EnvGuard guard({
        {"DRAGON_AUDIO_SINK_PLUGIN_DIR", emptyPluginDir.path().toUtf8()},
        {"DRAGON_AUDIO_SINK", QByteArray()},
    });

    // Auto with no plugins available anywhere
    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<NullAudioSink *>(pipeline->sink()) != nullptr);
    QVERIFY(pipeline->probe());
    QVERIFY(!pipeline->isDeviceOpen());

    // A specifically requested backend is also unavailable: still no nullptr
    selectedSink = DragonAudioOutput::Backend::Auto;
    pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::SDL, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<NullAudioSink *>(pipeline->sink()) != nullptr);

    // A DRAGON_AUDIO_SINK request for a plugin that does not exist: still no nullptr
    qputenv("DRAGON_AUDIO_SINK", "dragonsdlaudiosink");
    selectedSink = DragonAudioOutput::Backend::Auto;
    pipeline = createPlaybackPipeline(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(pipeline != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<NullAudioSink *>(pipeline->sink()) != nullptr);
}

void TestSinkNull::testNullSinkMethods()
{
    PlaybackPipeline pipeline(std::make_unique<NullAudioSink>());
    QVERIFY(pipeline.probe());
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(!pipeline.isPaused());
    QCOMPARE(pipeline.deviceQueuedSamples(), 0);

    // Opening records the format and reports the broken install, but the
    // device never becomes open: that is the contract DragonPlayer's generic
    // open-failure handling relies on to refuse playback.
    Pipe<float> pipe;
    pipeline.setAudioPipe(&pipe);

    QSignalSpy errorSpy(&pipeline, &PlaybackPipeline::errorOccurred);
    pipeline.open(44100, 2);
    QVERIFY(!pipeline.isDeviceOpen());
    QVERIFY(!pipeline.isPaused());
    QVERIFY(pipeline.hasFormat(44100, 2));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.last().at(0).toString().contains(u"plugins"_s, Qt::CaseInsensitive));

    // Volume and mute operations
    pipeline.setVolume(0.5f);
    QVERIFY(qAbs(pipeline.volume() - 0.5f) < 0.01f);
    pipeline.setMuted(true);
    QVERIFY(pipeline.muted());

    // Pause / resume / close
    pipeline.pause();
    pipeline.resume();
    pipeline.close();
    pipeline.clearStream();
}

void TestSinkNull::testNullSinkSignals()
{
    PlaybackPipeline pipeline(std::make_unique<NullAudioSink>());
    QSignalSpy errorSpy(&pipeline, &PlaybackPipeline::errorOccurred);
    QVERIFY(errorSpy.wait(200));
    QString msg = errorSpy.takeFirst().at(0).toString();
    QVERIFY(msg.contains(u"plugins"_s, Qt::CaseInsensitive));
}

void TestSinkNull::testPlayerWithNullSinkState()
{
    VERIFY_FIXTURE_EXISTS(u"sample-3s.mp3"_s);

    DragonPlayer player(DragonAudioOutput::Backend::Null);

    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    // Process queued events to receive initial error from null device
    if (player.error() == DragonPlayer::Error::NoError) {
        errorSpy.wait(100);
    }

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.error(), DragonPlayer::Error::ResourceError);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::InvalidMedia);
    QVERIFY(player.errorString().contains(u"plugins"_s, Qt::CaseInsensitive));

    // play() without a source is a no-op
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    // Load a real file and request playback: the media loads successfully,
    // but the device's open() fails by contract (isDeviceOpen() stays false),
    // so the player must refuse to enter PlayingState and keep reporting the
    // resource error instead of silently pretending to play.
    QSignalSpy stateSpy(&player, &DragonPlayer::playbackStateChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(DRAGON_SDL_TESTS_FIXTURES_DIR) + u"/sample-3s.mp3"_s));
    player.play();

    // LoadingMedia -> LoadedMedia -> InvalidMedia (the playback gate re-sets
    // the error once the load succeeds)
    QTRY_VERIFY(statusSpy.count() >= 3);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::InvalidMedia);
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.error(), DragonPlayer::Error::ResourceError);
    QVERIFY(player.errorString().contains(u"plugins"_s, Qt::CaseInsensitive));
    QVERIFY(stateSpy.isEmpty());

    player.pause();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    player.stop();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QVERIFY(stateSpy.isEmpty());
}

QTEST_GUILESS_MAIN(TestSinkNull)
#include "test_sink_null.moc"
