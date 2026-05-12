/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Common test utilities for dragon-sdl test suite.
 * This header provides helper classes and functions to reduce
 * boilerplate in test files while maintaining full test coverage.
 */

#pragma once

#include <QFileInfo>
#include <QSignalSpy>
#include <QTimer>
#include <QUrl>
#include <QtCore>
#include <QtTest>

#include "dragondecoder.h"
#include <dragonsdl/dragonplayer.h>

#include <atomic>
#include <ranges>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;

class TestFixture
{
public:
    static QString fixturePath(const QString &filename)
    {
        QString fixturesDir = QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR);
        return fixturesDir + u"/" + filename;
    }

    static QString fixturesDir()
    {
        return QString::fromLocal8Bit(DRAGON_SDL_TESTS_FIXTURES_DIR);
    }

    static bool verifyExists(const QString &filename, const char *file = __FILE__, int line = __LINE__)
    {
        QString path = fixturePath(filename);
        bool exists = QFileInfo::exists(path);
        if (!exists) {
            QTest::qFail(qPrintable(u"Fixture not found: %1"_s.arg(path)), file, line);
        }
        return exists;
    }

    static QStringList allAudioFixtures()
    {
        return {u"sample-3s.mp3"_s,
                u"sample-3s.aac"_s,
                u"gs-16b-2c-44100hz.ogg"_s,
                u"gs-16b-1c-44100hz.flac"_s,
                u"gs-16b-2c-44100hz.m4a"_s,
                u"gs-16b-1c-44100hz.wma"_s};
    }

    static std::vector<std::tuple<QString, int, int>> formattedFixtures()
    {
        return {{u"sample-3s.mp3"_s, 44100, -1},
                {u"sample-3s.aac"_s, 44100, -1},
                {u"gs-16b-2c-44100hz.ogg"_s, 44100, 2},
                {u"gs-16b-1c-44100hz.flac"_s, 44100, 1},
                {u"gs-16b-2c-44100hz.m4a"_s, 44100, 2},
                {u"gs-16b-1c-44100hz.wma"_s, 44100, 1}};
    }
    static QStringList stereoFixtures()
    {
        return {u"gs-16b-2c-44100hz.ogg"_s, u"gs-16b-2c-44100hz.m4a"_s};
    }

    static QStringList shortFixtures()
    {
        return {u"sample-3s.mp3"_s, u"sample-3s.aac"_s};
    }
};

class PlayerHelper
{
public:
    explicit PlayerHelper(DragonPlayer *player)
        : m_player(player)
    {
    }

    bool setSourceAndWait(const QString &fixtureName, int timeoutMs = 10000)
    {
        QString path = TestFixture::fixturePath(fixtureName);
        if (!QFileInfo::exists(path)) {
            return false;
        }

        QSignalSpy statusSpy(m_player, &DragonPlayer::statusChanged);
        QSignalSpy errorSpy(m_player, &DragonPlayer::errorChanged);

        m_player->setSource(QUrl::fromLocalFile(path));

        return QTest::qWaitFor(
            [&]() {
                return m_player->status() == DragonPlayer::MediaStatus::LoadedMedia || errorSpy.count() > 0;
            },
            timeoutMs);
    }

    bool setSourceAndWait(const QUrl &url, int timeoutMs = 10000)
    {
        QSignalSpy statusSpy(m_player, &DragonPlayer::statusChanged);
        QSignalSpy errorSpy(m_player, &DragonPlayer::errorChanged);

        m_player->setSource(url);

        return QTest::qWaitFor(
            [&]() {
                return m_player->status() == DragonPlayer::MediaStatus::LoadedMedia || errorSpy.count() > 0;
            },
            timeoutMs);
    }

    bool playAndWait(int timeoutMs = 5000)
    {
        QSignalSpy stateSpy(m_player, &DragonPlayer::playbackStateChanged);

        m_player->play();

        return QTest::qWaitFor(
            [&]() {
                return m_player->playbackState() == DragonPlayer::PlaybackState::PlayingState;
            },
            timeoutMs);
    }

    bool pauseAndWait(int timeoutMs = 2000)
    {
        m_player->pause();

        return QTest::qWaitFor(
            [&]() {
                return m_player->playbackState() == DragonPlayer::PlaybackState::PausedState;
            },
            timeoutMs);
    }

    bool stopAndWait(int timeoutMs = 2000)
    {
        m_player->stop();

        return QTest::qWaitFor(
            [&]() {
                return m_player->playbackState() == DragonPlayer::PlaybackState::StoppedState;
            },
            timeoutMs);
    }

    void setNextSource(const QString &fixtureName)
    {
        QString path = TestFixture::fixturePath(fixtureName);
        m_player->setNextSource(QUrl::fromLocalFile(path));
    }

    bool waitForTrackChange(int timeoutMs = 30000)
    {
        QSignalSpy trackSpy(m_player, &DragonPlayer::trackChanged);
        return QTest::qWaitFor(
            [&]() {
                return trackSpy.count() > 0;
            },
            timeoutMs);
    }

    bool waitForEndOfMedia(int timeoutMs = 15000)
    {
        return QTest::qWaitFor(
            [&]() {
                return m_player->status() == DragonPlayer::MediaStatus::EndOfMedia;
            },
            timeoutMs);
    }

    bool verifyNoStopState(QSignalSpy &stateSpy) const
    {
        for (const auto &args : stateSpy) {
            auto state = args.at(0).value<DragonPlayer::PlaybackState>();
            if (state == DragonPlayer::PlaybackState::StoppedState) {
                return false;
            }
        }
        return true;
    }

    bool verifyNoEndOfMedia(QSignalSpy &statusSpy) const
    {
        for (const auto &args : statusSpy) {
            auto status = args.at(0).value<DragonPlayer::MediaStatus>();
            if (status == DragonPlayer::MediaStatus::EndOfMedia) {
                return false;
            }
        }
        return true;
    }

    bool wasAudioEverInactive(int durationMs, int pollIntervalMs = 10)
    {
        std::atomic<bool> audioEverInactive{false};

        QTimer pollTimer;
        QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
            if (!m_player->isAudioActive()) {
                audioEverInactive.store(true);
            }
        });
        pollTimer.start(pollIntervalMs);

        QTest::qWait(durationMs);
        pollTimer.stop();

        return audioEverInactive.load();
    }

    DragonPlayer *player() const
    {
        return m_player;
    }

private:
    DragonPlayer *m_player;
};

class SignalSpyHelper
{
public:
    static QSignalSpy statusSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::statusChanged);
    }

    static QSignalSpy stateSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::playbackStateChanged);
    }

    static QSignalSpy sourceSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::sourceChanged);
    }

    static QSignalSpy trackSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::trackChanged);
    }

    static QSignalSpy errorSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::errorChanged);
    }

    static QSignalSpy durationSpy(DragonPlayer *player)
    {
        return QSignalSpy(player, &DragonPlayer::durationChanged);
    }

    static QSignalSpy formatSpy(DragonDecoder *decoder)
    {
        return QSignalSpy(decoder, &DragonDecoder::formatReady);
    }

    static QSignalSpy decoderErrorSpy(DragonDecoder *decoder)
    {
        return QSignalSpy(decoder, &DragonDecoder::streamError);
    }

    static QSignalSpy decoderDurationSpy(DragonDecoder *decoder)
    {
        return QSignalSpy(decoder, &DragonDecoder::durationChanged);
    }
};

class FftFrameCounter
{
public:
    explicit FftFrameCounter(DragonPlayer *player)
    {
        QObject::connect(
            player,
            &DragonPlayer::fftFrameReady,
            player,
            [this]() {
                m_count.fetch_add(1);
            },
            Qt::QueuedConnection);
    }

    int count() const
    {
        return m_count.load();
    }

    void reset()
    {
        m_count.store(0);
    }

private:
    std::atomic<int> m_count{0};
};

#define VERIFY_FIXTURE_EXISTS(filename) QVERIFY2(QFileInfo::exists(TestFixture::fixturePath(filename)), qPrintable(u"Fixture not found: %1"_s.arg(filename)))

#define VERIFY_DECODE_SUCCESS(result, filename) QVERIFY2(!(result).hadError, qPrintable(u"Decode failed for %1: %2"_s.arg(filename).arg((result).errorMessage)))

#define VERIFY_PLAYER_LOADED(player, filename)                                                                                                                 \
    QVERIFY2((player).status() == DragonPlayer::MediaStatus::LoadedMedia, qPrintable(u"Failed to load: %1"_s.arg(filename)))

#define VERIFY_PLAYING_STATE(player)                                                                                                                           \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::PlayingState, u"Expected PlayingState"_s.toUtf8().constData())
#define VERIFY_PAUSED_STATE(player)                                                                                                                            \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::PausedState, u"Expected PausedState"_s.toUtf8().constData())

#define VERIFY_STOPPED_STATE(player)                                                                                                                           \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::StoppedState, u"Expected StoppedState"_s.toUtf8().constData())

#define VERIFY_AUDIO_ACTIVE(player) QVERIFY2((player).isAudioActive(), u"Audio should be active"_s.toUtf8().constData())

#define VERIFY_AUDIO_INACTIVE(player) QVERIFY2(!(player).isAudioActive(), u"Audio should be inactive"_s.toUtf8().constData())
