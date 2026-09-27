/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include <QTemporaryDir>

#include "test_utils.h"

#include "DragonMediaBackend/dragonaudiooutput.h"
#include "DragonMediaBackend/dragonplayer.h"
#include "sink/dragonaudiosink.h"
#include "sink/dragonaudiosinkfactory.h"
#include "sink/dragonnullaudiosink.h"

using namespace Qt::StringLiterals;

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
    auto sink = createAudioSink(DragonAudioOutput::Backend::Null, &selectedSink);
    QVERIFY(sink != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(sink->probe());
    QVERIFY(!sink->isDeviceOpen());
    QVERIFY(!sink->isPaused());
}

void TestSinkNull::testFactoryEnvRequestsNull()
{
    EnvGuard guard({{"DRAGON_AUDIO_SINK", QByteArray("dragonnullaudiosink")}});

    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto sink = createAudioSink(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(sink != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
}

void TestSinkNull::testFactoryFallbackNoPluginsFound()
{
    // Simulate a broken install where no audio-sink plugins can be found at
    // all: KPluginMetaData::findPlugins() always checks the application
    // directory, so from within the test binary the real plugins are always
    // visible. Pointing DRAGON_AUDIO_SINK_PLUGIN_DIR at an absolute path to
    // an empty directory makes findPlugins() look only there, i.e. exactly
    // the "no plugins found" situation the null sink exists for.
    QTemporaryDir emptyPluginDir;
    QVERIFY(emptyPluginDir.isValid());
    QVERIFY(QDir(emptyPluginDir.path()).mkpath(u"DragonMediaBackend/AudioSink"_s));

    EnvGuard guard({
        {"DRAGON_AUDIO_SINK_PLUGIN_DIR", emptyPluginDir.path().toUtf8()},
        {"DRAGON_AUDIO_SINK", QByteArray()},
    });

    // Auto with no plugins available anywhere
    DragonAudioOutput::Backend selectedSink = DragonAudioOutput::Backend::Auto;
    auto sink = createAudioSink(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(sink != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<DragonNullAudioSink *>(sink.get()) != nullptr);
    QVERIFY(sink->probe());
    QVERIFY(!sink->isDeviceOpen());

    // A specifically requested backend is also unavailable: still no nullptr
    selectedSink = DragonAudioOutput::Backend::Auto;
    sink = createAudioSink(DragonAudioOutput::Backend::SDL, &selectedSink);
    QVERIFY(sink != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<DragonNullAudioSink *>(sink.get()) != nullptr);

    // A DRAGON_AUDIO_SINK request for a plugin that does not exist: still no nullptr
    qputenv("DRAGON_AUDIO_SINK", "dragonsdlaudiosink");
    selectedSink = DragonAudioOutput::Backend::Auto;
    sink = createAudioSink(DragonAudioOutput::Backend::Auto, &selectedSink);
    QVERIFY(sink != nullptr);
    QCOMPARE(selectedSink, DragonAudioOutput::Backend::Null);
    QVERIFY(qobject_cast<DragonNullAudioSink *>(sink.get()) != nullptr);
}

void TestSinkNull::testNullSinkMethods()
{
    DragonNullAudioSink sink;
    QVERIFY(sink.probe());
    QVERIFY(!sink.isDeviceOpen());
    QVERIFY(!sink.isPaused());
    QCOMPARE(sink.deviceQueuedSamples(), 0);

    // Opening should set format and emit error
    QSignalSpy errorSpy(&sink, &DragonAudioSink::errorOccurred);
    sink.open(44100, 2);
    QVERIFY(!sink.isDeviceOpen());
    QVERIFY(!sink.isPaused());
    QVERIFY(sink.hasFormat(44100, 2));
    QVERIFY(errorSpy.count() >= 1);
    QVERIFY(errorSpy.last().at(0).toString().contains(u"plugins"_s, Qt::CaseInsensitive));

    // Volume and mute operations
    sink.setVolume(0.5f);
    QVERIFY(qAbs(sink.volume() - 0.5f) < 0.01f);
    sink.setMuted(true);
    QVERIFY(sink.muted());

    // Pause / resume / close
    sink.pause();
    sink.resume();
    sink.close();
    sink.clearStream();
}

void TestSinkNull::testNullSinkSignals()
{
    DragonNullAudioSink sink;
    QSignalSpy errorSpy(&sink, &DragonAudioSink::errorOccurred);
    QVERIFY(errorSpy.wait(200));
    QString msg = errorSpy.takeFirst().at(0).toString();
    QVERIFY(msg.contains(u"plugins"_s, Qt::CaseInsensitive));
}

void TestSinkNull::testPlayerWithNullSinkState()
{
    DragonPlayer player(DragonAudioOutput::Backend::Null);

    QSignalSpy errorSpy(&player, &DragonPlayer::errorChanged);

    // Process queued events to receive initial error from null sink
    if (player.error() == DragonPlayer::Error::NoError) {
        errorSpy.wait(100);
    }

    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    QCOMPARE(player.error(), DragonPlayer::Error::ResourceError);
    QCOMPARE(player.status(), DragonPlayer::MediaStatus::InvalidMedia);
    QVERIFY(player.errorString().contains(u"plugins"_s, Qt::CaseInsensitive));

    // Attempting to play should keep playback state stopped and remain in error
    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    // Setting a source
    player.setSource(QUrl::fromLocalFile(QString::fromUtf8(DRAGON_SDL_TESTS_FIXTURES_DIR) + u"/sine_440_cbr.mp3"_s));
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);

    player.play();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    player.pause();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
    player.stop();
    QCOMPARE(player.playbackState(), DragonPlayer::PlaybackState::StoppedState);
}

QTEST_GUILESS_MAIN(TestSinkNull)
#include "test_sink_null.moc"
