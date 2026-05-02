/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>

class DragonPlayer;
class DragonPlaylist;
class QPushButton;
class QSlider;
class QLabel;
class QListWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void openFile();
    void openMultipleFiles();
    void clearPlaylist();
    void playPlaylistItem(int index);
    void updatePlaybackState();
    void updatePosition(int64_t positionMs);
    void updateDuration(int64_t durationMs);
    void setPositionFromSlider();
    void setVolumeFromSlider(int value);
    void updateStatus();
    void updatePlaylistCurrentIndex(int index);

private:
    void setupUi();
    void connectPlayer();

    [[nodiscard]] static QString formatTime(int64_t ms);

    DragonPlayer *m_player = nullptr;
    DragonPlaylist *m_playlist = nullptr;

    QPushButton *m_playButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_prevButton = nullptr;

    QSlider *m_seekSlider = nullptr;
    QLabel *m_timeLabel = nullptr;

    QSlider *m_volumeSlider = nullptr;

    QListWidget *m_playlistWidget = nullptr;
    QLabel *m_statusLabel = nullptr;
    bool m_seeking = false;
    int64_t m_durationMs = 0;
};
