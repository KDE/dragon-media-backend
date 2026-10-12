/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplaylist.h"

#include <DragonPlayer>

#include <QDebug>

DragonPlaylist::DragonPlaylist(DragonPlayer *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
    QObject::connect(player, &DragonPlayer::trackChanged, this, [this]() {
        if (m_gaplessEnabled && m_currentIndex >= 0 && m_currentIndex < m_tracks.size() - 1) {
            m_currentIndex++;
            qDebug() << "PLAYLIST: seamless advance to index" << m_currentIndex;
            Q_EMIT currentIndexChanged(m_currentIndex);
            updatePlayerQueue();
        }
    });

    QObject::connect(player, &DragonPlayer::statusChanged, this, [this](DragonPlayer::MediaStatus status) {
        if (status == DragonPlayer::MediaStatus::EndOfMedia) {
            if (m_currentIndex >= 0 && m_currentIndex < m_tracks.size() - 1) {
                qDebug() << "PLAYLIST: track ended (non-gapless), advancing to index" << m_currentIndex + 1;
                playNext();
            } else if (m_currentIndex >= 0 && m_currentIndex == m_tracks.size() - 1) {
                qDebug() << "PLAYLIST: track ended, at end of playlist";
            }
        }
    });

    QObject::connect(player, &DragonPlayer::sourceChanged, this, [this]() {
        syncIndexFromPlayer();
    });
}

void DragonPlaylist::addTrack(const QUrl &url)
{
    addEntries({Entry{url, url.fileName(), {}, {}}});
}

void DragonPlaylist::addTracks(const QList<QUrl> &urls)
{
    QList<Entry> entries;
    entries.reserve(urls.size());
    for (const QUrl &url : urls) {
        entries.append(Entry{url, url.fileName(), {}, {}});
    }
    addEntries(entries);
}

void DragonPlaylist::addEntries(const QList<Entry> &entries)
{
    if (entries.isEmpty()) {
        return;
    }

    const bool wasEmpty = m_tracks.isEmpty();
    m_tracks.append(entries);

    if (wasEmpty) {
        m_currentIndex = 0;
        m_player->setSource(m_tracks.first().url);
        m_player->play();
        Q_EMIT currentIndexChanged(0);
    }

    updatePlayerQueue();
    Q_EMIT tracksChanged();
}

void DragonPlaylist::clear()
{
    m_tracks.clear();
    m_currentIndex = -1;
    m_player->setSource(QUrl{});
    m_player->setNextSource(QUrl{});
    Q_EMIT currentIndexChanged(-1);
    Q_EMIT tracksChanged();
}

void DragonPlaylist::removeTrack(int index)
{
    if (index < 0 || index >= m_tracks.size()) {
        return;
    }

    m_tracks.removeAt(index);

    if (m_tracks.isEmpty()) {
        m_currentIndex = -1;
        m_player->setSource(QUrl{});
        m_player->setNextSource(QUrl{});
    } else if (index == m_currentIndex) {
        m_currentIndex = qMin(index, m_tracks.size() - 1);
        m_player->setSource(m_tracks[m_currentIndex].url);
    } else if (index < m_currentIndex) {
        m_currentIndex--;
    }

    updatePlayerQueue();
    Q_EMIT currentIndexChanged(m_currentIndex);
    Q_EMIT tracksChanged();
}

void DragonPlaylist::setEntryMetadata(int index, const QString &title, const QString &artist)
{
    if (index < 0 || index >= m_tracks.size()) {
        return;
    }

    m_tracks[index].title = title;
    m_tracks[index].artist = artist;
    Q_EMIT entryMetadataChanged(index);
}

QList<QUrl> DragonPlaylist::tracks() const
{
    QList<QUrl> urls;
    urls.reserve(m_tracks.size());
    for (const Entry &entry : m_tracks) {
        urls.append(entry.url);
    }
    return urls;
}

DragonPlaylist::Entry DragonPlaylist::entryAt(int index) const
{
    if (index < 0 || index >= m_tracks.size()) {
        return {};
    }
    return m_tracks.at(index);
}

int DragonPlaylist::currentIndex() const
{
    return m_currentIndex;
}

int DragonPlaylist::count() const
{
    return m_tracks.size();
}

QUrl DragonPlaylist::trackAt(int index) const
{
    if (index < 0 || index >= m_tracks.size()) {
        return {};
    }
    return m_tracks.at(index).url;
}

void DragonPlaylist::playNext()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_tracks.size() - 1) {
        return;
    }
    m_currentIndex++;
    m_player->setSource(m_tracks[m_currentIndex].url);
    m_player->play();
    Q_EMIT currentIndexChanged(m_currentIndex);
    updatePlayerQueue();
}

void DragonPlaylist::playPrevious()
{
    if (m_currentIndex <= 0) {
        return;
    }
    m_currentIndex--;
    m_player->setSource(m_tracks[m_currentIndex].url);
    m_player->play();
    Q_EMIT currentIndexChanged(m_currentIndex);
    updatePlayerQueue();
}

void DragonPlaylist::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_tracks.size() || index == m_currentIndex) {
        return;
    }
    m_currentIndex = index;
    m_player->setSource(m_tracks[m_currentIndex].url);
    m_player->play();
    Q_EMIT currentIndexChanged(m_currentIndex);
    updatePlayerQueue();
}

bool DragonPlaylist::gaplessEnabled() const
{
    return m_gaplessEnabled;
}

void DragonPlaylist::setGaplessEnabled(bool enabled)
{
    if (m_gaplessEnabled != enabled) {
        m_gaplessEnabled = enabled;
        Q_EMIT gaplessEnabledChanged(enabled);
        updatePlayerQueue();
    }
}

void DragonPlaylist::updatePlayerQueue()
{
    if (m_gaplessEnabled && m_currentIndex >= 0 && m_currentIndex < m_tracks.size() - 1) {
        m_player->setNextSource(m_tracks[m_currentIndex + 1].url);
        qDebug() << "PLAYLIST: queued next track" << m_tracks[m_currentIndex + 1].url.toString();
    } else {
        m_player->setNextSource(QUrl{});
        if (m_gaplessEnabled) {
            qDebug() << "PLAYLIST: no next track to queue";
        } else {
            qDebug() << "PLAYLIST: gapless disabled, not queuing next track";
        }
    }
}

void DragonPlaylist::syncIndexFromPlayer()
{
    QUrl current = m_player->source();
    int idx = -1;
    for (int i = 0; i < m_tracks.size(); ++i) {
        if (m_tracks[i].url == current) {
            idx = i;
            break;
        }
    }
    if (idx >= 0 && idx != m_currentIndex) {
        m_currentIndex = idx;
        Q_EMIT currentIndexChanged(m_currentIndex);
        updatePlayerQueue();
    }
}
