/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <QtCore>
#include <QtTest>

#include "helpers/pa_sink_input_props.h"
#include "helpers/pw_node_props.h"
#include "logging_timestamp_init.h"
#include "test_utils.h"

#include <DragonMediaBackend/dragonplayer.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QStringList>
#include <SDL3/SDL_version.h>

#include <optional>

using namespace Qt::StringLiterals;

namespace
{
bool sdlSupportsLiveStreamNameRename()
{
    return SDL_GetVersion() >= SDL_VERSIONNUM(3, 6, 0);
}
} // namespace

class TestPlayerStreamName : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOpenTimeSemantics();
    void testLiveUpdateSemantics();

private:
    [[nodiscard]] static QString currentSinkBackend();
};

QString TestPlayerStreamName::currentSinkBackend()
{
    return qEnvironmentVariable("DRAGON_AUDIO_SINK");
}

void TestPlayerStreamName::testOpenTimeSemantics()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonsdlaudiosink"_s && backend != u"dragonpulseaudiosink"_s && backend != u"dragonpipewireaudiosink"_s) {
        QSKIP("Stream-name player semantics require the SDL, PulseAudio, or PipeWire backend");
    }
    const bool viaPulse = backend == u"dragonpulseaudiosink"_s;
    const bool viaSdl = backend == u"dragonsdlaudiosink"_s;

    const QString uniqueTag = u"DragonPlayerStreamName_%1"_s.arg(QCoreApplication::applicationPid());
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

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    if (!QTest::qWaitFor(
            [&serverProps]() {
                return serverProps().has_value();
            },
            5000)) {
        if (viaSdl) {
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

    QVERIFY(helper.stopAndWait());
    QVERIFY2(QTest::qWaitFor(
                 [&serverProps]() {
                     return !serverProps().has_value();
                 },
                 5000),
             "a closed stream should disappear from the audio server");

    player.setStreamName(u"Seeded Track"_s);
    QVERIFY(helper.playAndWait());
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp]() {
                     return serverProp(u"media.name"_s) == u"Seeded Track"_s;
                 },
                 5000),
             "setStreamName() while stopped should seed media.name when playback reopens the stream");

    player.setStreamName(QString());
    if (viaSdl) {
        QVERIFY2(QTest::qWaitFor(
                     [&serverProp]() {
                         return serverProp(u"media.name"_s) == u"Seeded Track"_s;
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

    QVERIFY(helper.stopAndWait());
}

void TestPlayerStreamName::testLiveUpdateSemantics()
{
    const QString backend = currentSinkBackend();
    if (backend != u"dragonsdlaudiosink"_s && backend != u"dragonpulseaudiosink"_s && backend != u"dragonpipewireaudiosink"_s) {
        QSKIP("Stream-name player semantics require the SDL, PulseAudio, or PipeWire backend");
    }
    const bool viaPulse = backend == u"dragonpulseaudiosink"_s;
    const bool viaSdl = backend == u"dragonsdlaudiosink"_s;
    if (viaSdl && !sdlSupportsLiveStreamNameRename()) {
        const int linkedSdlVersion = SDL_GetVersion();
        QSKIP(u"live stream-name updates require SDL 3.6.0+; this SDL is %1.%2.%3"_s.arg(SDL_VERSIONNUM_MAJOR(linkedSdlVersion))
                  .arg(SDL_VERSIONNUM_MINOR(linkedSdlVersion))
                  .arg(SDL_VERSIONNUM_MICRO(linkedSdlVersion))
                  .toUtf8()
                  .constData());
    }

    const QString uniqueTag = u"DragonPlayerStreamName_%1"_s.arg(QCoreApplication::applicationPid());
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

    DragonPlayer player;
    PlayerHelper helper(&player);

    QVERIFY(helper.setSourceAndWait(u"sample-3s.mp3"_s));
    QVERIFY(helper.playAndWait());

    if (!QTest::qWaitFor(
            [&serverProps]() {
                return serverProps().has_value();
            },
            5000)) {
        if (viaSdl) {
            QSKIP("SDL stream not visible in PipeWire (SDL audio not using its PipeWire driver)");
        }
        QFAIL("no stream with the test application name visible on the audio server");
    }

    player.setStreamName(u"Live Track A"_s);
    QVERIFY2(QTest::qWaitFor(
                 [&serverProp]() {
                     return serverProp(u"media.name"_s) == u"Live Track A"_s;
                 },
                 5000),
             "setStreamName() during playback should update media.name live");

    if (!viaSdl) {
        QVERIFY(helper.setSourceAndWait(u"gs-16b-2c-44100hz.ogg"_s));
        QVERIFY2(QTest::qWaitFor(
                     [&serverProp, &uniqueTag]() {
                         return serverProp(u"media.name"_s) == uniqueTag;
                     },
                     5000),
                 "setSource() to a new track should invalidate the previous stream name");

        player.setStreamName(u"Track B"_s);
        QVERIFY2(QTest::qWaitFor(
                     [&serverProp]() {
                         return serverProp(u"media.name"_s) == u"Track B"_s;
                     },
                     5000),
                 "stream name should be pushable for the new track");
    }

    QVERIFY(helper.stopAndWait());
}

QTEST_MAIN(TestPlayerStreamName)
#include "test_player_streamname.moc"
