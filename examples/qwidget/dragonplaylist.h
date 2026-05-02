/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class DragonPlayer;

class DragonPlaylist : public QObject
{
    Q_OBJECT

public:
    explicit DragonPlaylist(DragonPlayer *player, QObject *parent = nullptr);

    void addTrack(const QUrl &url);
    void addTracks(const QList<QUrl> &urls);
    void clear();
    void removeTrack(int index);

    [[nodiscard]] QList<QUrl> tracks() const;
    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] int count() const;
    [[nodiscard]] QUrl trackAt(int index) const;

    void playNext();
    void playPrevious();
    void setCurrentIndex(int index);

Q_SIGNALS:
    void currentIndexChanged(int index);
    void tracksChanged();

private:
    void updatePlayerQueue();
    void syncIndexFromPlayer();

    DragonPlayer *m_player = nullptr;
    QList<QUrl> m_tracks;
    int m_currentIndex = -1;
};
