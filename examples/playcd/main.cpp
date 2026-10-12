/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <DragonCdToc>
#include <DragonCdTocReader>
#include <DragonCdTrack>
#include <DragonPlayer>

#include <QCoreApplication>
#include <QDebug>
#include <QStringLiteral>
#include <QTimer>
#include <QUrl>

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

QString formatDuration(std::chrono::milliseconds duration)
{
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    return QStringLiteral("%1:%2").arg(totalSeconds / 60, 2, 10, QLatin1Char('0')).arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

// Plays every audio track of the disc in TOC order with gapless
// transitions, and stops cleanly after the final track because the
// track list is known up front from the TOC.
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
        connect(m_player, &DragonPlayer::errorChanged, this, &CdAutoPlayer::onErrorChanged);
    }

    void start()
    {
        auto *reader = new DragonCdTocReader(this);
        connect(reader, &DragonCdTocReader::tocReady, this, [this](const DragonCdToc &toc) {
            m_audioTracks.clear();
            for (const DragonCdTrack &track : toc.tracks()) {
                if (track.isAudio()) {
                    m_audioTracks.append(track);
                }
            }
            if (m_audioTracks.isEmpty()) {
                qWarning() << "The disc has no audio tracks.";
                QCoreApplication::exit(1);
                return;
            }

            qInfo() << "Disc:" << toc.tracks().size() << "tracks," << formatDuration(toc.discDuration());
            for (const DragonCdTrack &track : toc.tracks()) {
                qInfo() << " track" << track.number() << (track.isAudio() ? "audio" : "data") << formatDuration(track.duration());
            }

            m_player->setSource(m_audioTracks.first().url());
            m_player->play();
        });
        connect(reader, &DragonCdTocReader::failed, this, [](const QString &errorMessage) {
            qWarning() << errorMessage;
            QCoreApplication::exit(1);
        });
        reader->requestToc();
    }

private:
    void queueNextTrack()
    {
        const int next = m_currentIndex + 1;
        if (next < m_audioTracks.size()) {
            m_player->setNextSource(m_audioTracks.at(next).url());
        }
    }

    void onSourceChanged()
    {
        const QUrl source = m_player->source();
        int index = -1;
        for (int i = 0; i < m_audioTracks.size(); ++i) {
            if (m_audioTracks.at(i).url() == source) {
                index = i;
                break;
            }
        }
        if (index >= 0 && index != m_currentIndex) {
            m_currentIndex = index;
            qInfo() << "Playing CD track" << m_audioTracks.at(index).number();
            if (index == m_audioTracks.size() - 1) {
                // Last track: no nextSource is queued, EndOfMedia ends
                // the run cleanly.
                m_player->setNextSource(QUrl{});
            }
        }
    }

    void onErrorChanged(DragonPlayer::Error error)
    {
        if (error != DragonPlayer::Error::NoError) {
            qWarning() << "Stopping:" << m_player->errorString();
            QCoreApplication::exit(1);
        }
    }

    DragonPlayer *m_player = nullptr;
    QList<DragonCdTrack> m_audioTracks;
    int m_currentIndex = -1;
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
