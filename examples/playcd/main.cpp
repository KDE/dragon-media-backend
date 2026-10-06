/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <DragonPlayer>

#include <QCoreApplication>
#include <QDebug>
#include <QStringLiteral>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <chrono>
#include <csignal>

#include <atomic>

namespace
{
std::atomic<bool> terminateRequested{false};

void installSignalHandlers()
{
    const auto forward = [](int) {
        terminateRequested.store(true);
    };
    std::signal(SIGINT, forward);
    std::signal(SIGTERM, forward);
}

QUrl cdTrackUrl(int track)
{
    QUrl url;
    url.setScheme(QStringLiteral("audiocd"));
    url.setPath(QStringLiteral("/Track %1.wav").arg(track, 2, 10, QLatin1Char('0')));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("track"), QString::number(track));
    url.setQuery(query);
    return url;
}

int trackFromUrl(const QUrl &url)
{
    bool ok = false;
    const int track = QUrlQuery(url).queryItemValue(QStringLiteral("track")).toInt(&ok);
    return ok ? track : 0;
}

QString formatDuration(std::chrono::milliseconds duration)
{
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return QStringLiteral("%1:%2").arg(totalSeconds / 60, 2, 10, QLatin1Char('0')).arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

class CdAutoPlayer : public QObject
{
    Q_OBJECT

public:
    explicit CdAutoPlayer(DragonPlayer *player, QObject *parent = nullptr)
        : QObject(parent)
        , m_player(player)
    {
        connect(m_player, &DragonPlayer::sourceChanged, this, &CdAutoPlayer::onSourceChanged);
        connect(m_player, &DragonPlayer::aboutToFinish, this, &CdAutoPlayer::queueNextTrack);
        connect(m_player, &DragonPlayer::statusChanged, this, &CdAutoPlayer::onStatusChanged);
        connect(m_player, &DragonPlayer::durationChanged, this, &CdAutoPlayer::onDurationChanged);
        connect(m_player, &DragonPlayer::errorChanged, this, &CdAutoPlayer::onErrorChanged);
    }

    void start()
    {
        playTrack(1);
    }

private:
    void playTrack(int track)
    {
        m_currentTrack = track;
        m_player->setSource(cdTrackUrl(track));
        m_player->play();
    }

    void queueNextTrack()
    {
        m_currentTrack = m_currentTrack + 1;
        m_player->setNextSource(cdTrackUrl(m_currentTrack));
    }

    void onSourceChanged()
    {
        const int track = trackFromUrl(m_player->source());
        if (track > 0 && track != m_announcedTrack) {
            m_announcedTrack = track;
            m_currentTrack = track;
            m_durationPrinted = false;
            qInfo() << "Playing CD track" << track;
        }
    }

    void onStatusChanged(DragonPlayer::MediaStatus status)
    {
        if (status == DragonPlayer::MediaStatus::EndOfMedia && m_player->nextSource().isEmpty()) {
            playTrack(m_currentTrack + 1);
        } else if (status == DragonPlayer::MediaStatus::InvalidMedia) {
            fail();
        }
    }

    void onDurationChanged(std::optional<std::chrono::milliseconds> duration)
    {
        if (!m_durationPrinted && duration) {
            m_durationPrinted = true;
            qInfo() << "Duration:" << formatDuration(*duration);
        }
    }

    void onErrorChanged(DragonPlayer::Error error)
    {
        if (error != DragonPlayer::Error::NoError) {
            fail();
        }
    }

    void fail()
    {
        qWarning() << "Stopping:" << m_player->errorString();
        QCoreApplication::exit(1);
    }

    DragonPlayer *m_player = nullptr;
    int m_currentTrack = 0;
    int m_announcedTrack = 0;
    bool m_durationPrinted = false;
};
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("dragonplaycd_example"));

    installSignalHandlers();

    DragonPlayer player;
    CdAutoPlayer autoPlayer(&player, &app);
    autoPlayer.start();

    QTimer ctrlCWatcher;
    QObject::connect(&ctrlCWatcher, &QTimer::timeout, &app, [&] {
        if (terminateRequested.exchange(false)) {
            qInfo() << "Interrupted, stopping.";
            player.stop();
            QCoreApplication::quit();
        }
    });
    ctrlCWatcher.start(std::chrono::milliseconds{200});

    return app.exec();
}

#include "main.moc"
