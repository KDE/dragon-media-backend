/**
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Common test utilities for Dragon Multimedia test suite.
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

#include "decoder/dragondecoder.h"
#include "player/dragondiagnostics.h"
#include <DragonMultimedia/dragonplayer.h>
#include <DragonMultimedia/dragonspectrumanalyzer.h>

#include <algorithm>
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

        const auto statusBefore = m_player->status();
        QSignalSpy statusSpy(m_player, &DragonPlayer::statusChanged);
        QSignalSpy errorSpy(m_player, &DragonPlayer::errorChanged);

        m_player->setSource(QUrl::fromLocalFile(path));

        // errorChanged(NoError) is emitted when loading clears stale error
        // state; only non-NoError emissions indicate a failed load.
        const auto hasRealError = [&errorSpy]() {
            return std::ranges::any_of(errorSpy, [](const QList<QVariant> &args) {
                return args.at(0).value<DragonPlayer::Error>() != DragonPlayer::Error::NoError;
            });
        };

        const bool reached = QTest::qWaitFor(
            [&]() {
                return m_player->status() == DragonPlayer::MediaStatus::LoadedMedia || m_player->status() == DragonPlayer::MediaStatus::BufferedMedia
                    || hasRealError();
            },
            timeoutMs);

        return reached && !hasRealError() && m_player->error() == DragonPlayer::Error::NoError && (statusSpy.count() > 0 || statusBefore == m_player->status());
    }

    bool setSourceAndWait(const QUrl &url, int timeoutMs = 10000)
    {
        const auto statusBefore = m_player->status();
        QSignalSpy statusSpy(m_player, &DragonPlayer::statusChanged);
        QSignalSpy errorSpy(m_player, &DragonPlayer::errorChanged);

        m_player->setSource(url);

        const auto hasRealError = [&errorSpy]() {
            return std::ranges::any_of(errorSpy, [](const QList<QVariant> &args) {
                return args.at(0).value<DragonPlayer::Error>() != DragonPlayer::Error::NoError;
            });
        };

        const bool reached = QTest::qWaitFor(
            [&]() {
                return m_player->status() == DragonPlayer::MediaStatus::LoadedMedia || m_player->status() == DragonPlayer::MediaStatus::BufferedMedia
                    || hasRealError();
            },
            timeoutMs);

        return reached && !hasRealError() && m_player->error() == DragonPlayer::Error::NoError && (statusSpy.count() > 0 || statusBefore == m_player->status());
    }

    bool playAndWait(int timeoutMs = 5000)
    {
        const auto stateBefore = m_player->playbackState();
        QSignalSpy stateSpy(m_player, &DragonPlayer::stateChanged);

        m_player->play();

        const bool reached = QTest::qWaitFor(
            [&]() {
                return m_player->playbackState() == DragonPlayer::PlaybackState::PlayingState;
            },
            timeoutMs);

        return reached && (stateSpy.count() > 0 || stateBefore == DragonPlayer::PlaybackState::PlayingState);
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

    bool waitForStatus(DragonPlayer::MediaStatus status, int timeoutMs = 10000)
    {
        return QTest::qWaitFor(
            [&]() {
                return m_player->status() == status;
            },
            timeoutMs);
    }

    bool waitForState(DragonPlayer::PlaybackState state, int timeoutMs = 5000)
    {
        return QTest::qWaitFor(
            [&]() {
                return m_player->playbackState() == state;
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
        DragonDiagnostics diagnostics(m_player);
        std::atomic<bool> audioEverInactive{false};

        QTimer pollTimer;
        QObject::connect(&pollTimer, &QTimer::timeout, [&]() {
            if (!diagnostics.isAudioActive()) {
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
        return QSignalSpy(player, &DragonPlayer::stateChanged);
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

    static QSignalSpy decoderErrorSpy(DragonDecoder *decoder)
    {
        return QSignalSpy(decoder, &DragonDecoder::streamError);
    }

    template<typename T>
    static bool contains(const QSignalSpy &spy, T value)
    {
        return std::ranges::any_of(spy, [value](const auto &args) {
            return args.at(0).template value<T>() == value;
        });
    }

    static bool containsState(const QSignalSpy &spy, DragonPlayer::PlaybackState state)
    {
        return contains(spy, state);
    }

    static bool containsTransition(const QSignalSpy &spy, DragonPlayer::PlaybackState newState, DragonPlayer::PlaybackState oldState)
    {
        return std::ranges::any_of(spy, [newState, oldState](const QList<QVariant> &args) {
            return args.at(0).value<DragonPlayer::PlaybackState>() == newState && args.at(1).value<DragonPlayer::PlaybackState>() == oldState;
        });
    }

    static bool containsStatus(const QSignalSpy &spy, DragonPlayer::MediaStatus status)
    {
        return contains(spy, status);
    }
};

class FftFrameCounter
{
public:
    explicit FftFrameCounter(DragonSpectrumAnalyzer *analyzer)
    {
        QObject::connect(
            analyzer,
            &DragonSpectrumAnalyzer::frameReady,
            analyzer,
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

    bool waitForFrames(int timeoutMs = 3000, int pollIntervalMs = 50)
    {
        int maxWaits = timeoutMs / pollIntervalMs;
        for (int i = 0; i < maxWaits && m_count.load() == 0; ++i) {
            QTest::qWait(pollIntervalMs);
        }
        return m_count.load() > 0;
    }

private:
    std::atomic<int> m_count{0};
};

class SignalOrderTracker
{
public:
    explicit SignalOrderTracker(DragonPlayer *player)
        : m_player(player)
    {
    }

    ~SignalOrderTracker()
    {
        cleanup();
    }

    void trackStateChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::stateChanged,
            m_player,
            [this](DragonPlayer::PlaybackState newState, DragonPlayer::PlaybackState oldState) {
                const auto stateName = [](DragonPlayer::PlaybackState s) -> QString {
                    switch (s) {
                    case DragonPlayer::PlaybackState::StoppedState:
                        return u"StoppedState"_s;
                    case DragonPlayer::PlaybackState::PlayingState:
                        return u"PlayingState"_s;
                    case DragonPlayer::PlaybackState::PausedState:
                        return u"PausedState"_s;
                    }
                    return u"Unknown"_s;
                };
                m_events.append(u"stateChanged("_s + stateName(newState) + u", "_s + stateName(oldState) + u")"_s);
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackStatusChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::statusChanged,
            m_player,
            [this](DragonPlayer::MediaStatus status) {
                switch (status) {
                case DragonPlayer::MediaStatus::LoadingMedia:
                    m_events.append(u"statusChanged(LoadingMedia)"_s);
                    break;
                case DragonPlayer::MediaStatus::LoadedMedia:
                    m_events.append(u"statusChanged(LoadedMedia)"_s);
                    break;
                case DragonPlayer::MediaStatus::NoMedia:
                    m_events.append(u"statusChanged(NoMedia)"_s);
                    break;
                case DragonPlayer::MediaStatus::EndOfMedia:
                    m_events.append(u"statusChanged(EndOfMedia)"_s);
                    break;
                case DragonPlayer::MediaStatus::InvalidMedia:
                    m_events.append(u"statusChanged(InvalidMedia)"_s);
                    break;
                default:
                    break;
                }
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackSourceChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::sourceChanged,
            m_player,
            [this]() {
                m_events.append(u"sourceChanged()"_s);
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackPositionZero()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::positionChanged,
            m_player,
            [this](qint64 pos) {
                if (pos == 0) {
                    m_events.append(u"positionChanged(0)"_s);
                }
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackDurationChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::durationChanged,
            m_player,
            [this](qint64 duration) {
                m_events.append(u"durationChanged("_s + QString::number(duration) + u")"_s);
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackErrorChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::errorChanged,
            m_player,
            [this](DragonPlayer::Error error) {
                m_events.append(u"errorChanged("_s + QString::number(static_cast<int>(error)) + u")"_s);
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    void trackPositionChanges()
    {
        auto conn = QObject::connect(
            m_player,
            &DragonPlayer::positionChanged,
            m_player,
            [this](qint64 pos) {
                m_events.append(u"positionChanged("_s + QString::number(pos) + u")"_s);
            },
            Qt::DirectConnection);
        m_connections.append(conn);
    }

    bool contains(const QString &event) const
    {
        return m_events.contains(event);
    }

    bool containsTransition(DragonPlayer::PlaybackState newState, DragonPlayer::PlaybackState oldState) const
    {
        const auto stateName = [](DragonPlayer::PlaybackState s) -> QString {
            switch (s) {
            case DragonPlayer::PlaybackState::StoppedState:
                return u"StoppedState"_s;
            case DragonPlayer::PlaybackState::PlayingState:
                return u"PlayingState"_s;
            case DragonPlayer::PlaybackState::PausedState:
                return u"PausedState"_s;
            }
            return u"Unknown"_s;
        };
        return m_events.contains(u"stateChanged("_s + stateName(newState) + u", "_s + stateName(oldState) + u")"_s);
    }

    bool containsPrefix(const QString &prefix) const
    {
        return std::ranges::any_of(m_events, [&prefix](const QString &e) {
            return e.startsWith(prefix);
        });
    }

    int indexOfPrefix(const QString &prefix) const
    {
        for (int i = 0; i < m_events.size(); ++i) {
            if (m_events[i].startsWith(prefix)) {
                return i;
            }
        }
        return -1;
    }

    bool verifyOrderPrefix(const QString &firstPrefix, const QString &secondPrefix) const
    {
        int firstIdx = indexOfPrefix(firstPrefix);
        int secondIdx = indexOfPrefix(secondPrefix);
        return firstIdx >= 0 && secondIdx >= 0 && firstIdx < secondIdx;
    }

    bool verifyOrder(const QString &first, const QString &second) const
    {
        int firstIdx = m_events.indexOf(first);
        int secondIdx = m_events.indexOf(second);
        return firstIdx >= 0 && secondIdx >= 0 && firstIdx < secondIdx;
    }

    const QStringList &events() const
    {
        return m_events;
    }

    void clear()
    {
        m_events.clear();
    }

private:
    DragonPlayer *m_player;
    QStringList m_events;
    QList<QMetaObject::Connection> m_connections;

    void cleanup()
    {
        for (const auto &conn : m_connections) {
            QObject::disconnect(conn);
        }
        m_connections.clear();
    }
};

#define VERIFY_FIXTURE_EXISTS(filename) QVERIFY2(QFileInfo::exists(TestFixture::fixturePath(filename)), qPrintable(u"Fixture not found: %1"_s.arg(filename)))

#define VERIFY_DECODE_SUCCESS(result, filename)                                                                                                                \
    do {                                                                                                                                                       \
        QVERIFY2(!(result).hadError, qPrintable(u"Decode failed for %1: %2"_s.arg(filename).arg((result).errorMessage)));                                      \
        QVERIFY2(!(result).sawUnexpectedFormatReady, qPrintable(u"Unexpected FormatReady event during decode of %1"_s.arg(filename)));                         \
    } while (false)

#define VERIFY_PLAYER_LOADED(player, filename)                                                                                                                 \
    QVERIFY2((player).status() == DragonPlayer::MediaStatus::LoadedMedia, qPrintable(u"Failed to load: %1"_s.arg(filename)))

#define VERIFY_PLAYING_STATE(player)                                                                                                                           \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::PlayingState, u"Expected PlayingState"_s.toUtf8().constData())

#define VERIFY_PAUSED_STATE(player)                                                                                                                            \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::PausedState, u"Expected PausedState"_s.toUtf8().constData())

#define VERIFY_STOPPED_STATE(player)                                                                                                                           \
    QVERIFY2((player).playbackState() == DragonPlayer::PlaybackState::StoppedState, u"Expected StoppedState"_s.toUtf8().constData())

#define VERIFY_AUDIO_ACTIVE(diagnostics) QVERIFY2((diagnostics).isAudioActive(), u"Audio should be active"_s.toUtf8().constData())

#define VERIFY_AUDIO_INACTIVE(diagnostics) QVERIFY2(!(diagnostics).isAudioActive(), u"Audio should be inactive"_s.toUtf8().constData())
#define VERIFY_POSITION_NEAR(actual, expected, tolerance)                                                                                                      \
    QVERIFY2(std::llabs(static_cast<qint64>(actual) - static_cast<qint64>(expected)) < static_cast<qint64>(tolerance),                                         \
             qPrintable(u"Position mismatch: expected ~%1ms, got %2ms (tolerance %3ms)"_s.arg(expected).arg(actual).arg(tolerance)))
