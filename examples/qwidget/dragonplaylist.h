/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <chrono>

class DragonPlayer;

class DragonPlaylist : public QObject
{
    Q_OBJECT

public:
    /*!
     * One playlist entry: the playable URL plus the metadata shown for
     * it. Titles start as placeholders ("Track 3", the file name) and
     * can be updated in place (for example when a CDDB lookup lands)
     * without touching the URL, so a title update mid-playback cannot
     * disturb the gapless queue.
     */
    struct Entry {
        QUrl url;
        QString title;
        QString artist;
        std::chrono::milliseconds duration{};
    };

    explicit DragonPlaylist(DragonPlayer *player, QObject *parent = nullptr);

    void addTrack(const QUrl &url);
    void addTracks(const QList<QUrl> &urls);
    void addEntries(const QList<Entry> &entries);
    void clear();
    void removeTrack(int index);

    void setEntryMetadata(int index, const QString &title, const QString &artist);

    [[nodiscard]] QList<QUrl> tracks() const;
    [[nodiscard]] Entry entryAt(int index) const;
    [[nodiscard]] int currentIndex() const;
    [[nodiscard]] int count() const;
    [[nodiscard]] QUrl trackAt(int index) const;

    [[nodiscard]] bool gaplessEnabled() const;
    void setGaplessEnabled(bool enabled);

    void playNext();
    void playPrevious();
    void setCurrentIndex(int index);

Q_SIGNALS:
    void currentIndexChanged(int index);
    void tracksChanged();
    void entryMetadataChanged(int index);
    void gaplessEnabledChanged(bool enabled);

private:
    void updatePlayerQueue();
    void syncIndexFromPlayer();

    DragonPlayer *m_player = nullptr;
    QList<Entry> m_tracks;
    int m_currentIndex = -1;
    bool m_gaplessEnabled = true;
};
