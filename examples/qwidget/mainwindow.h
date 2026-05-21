/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>
#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonicymetadata.h>

class DragonPlayer;
class DragonPlaylist;
class DragonVisualizer;
class DragonSpectrogram;
class QAction;
class QCheckBox;
class QPushButton;
class QSlider;
class QLabel;
class QListWidget;
class QLCDNumber;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private Q_SLOTS:
    void openFile();
    void openMultipleFiles();
    void addNetworkUrl();
    void clearPlaylist();
    void playKexp();
    void playLandSong();
    void playPlaylistItem(int index);
    void updatePlaybackState();
    void updatePosition(int64_t positionMs);
    void updateDuration(int64_t durationMs);
    void setPositionFromSlider();
    void setVolumeFromSlider(int value);
    void updateStatus();
    void updatePlaylistCurrentIndex(int index);
    void updateFftFrame(const DragonFftFrame &frame);
    void updateIcyMetadata(const DragonIcyMetadata &metadata);

private:
    void setupUi();
    void connectPlayer();

    bool eventFilter(QObject *obj, QEvent *event) override;

    [[nodiscard]] static QString formatTime(int64_t ms);

    DragonPlayer *m_player = nullptr;
    DragonPlaylist *m_playlist = nullptr;
    DragonVisualizer *m_visualizer = nullptr;
    DragonSpectrogram *m_spectrogram = nullptr;

    QPushButton *m_playButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_nextButton = nullptr;
    QPushButton *m_prevButton = nullptr;
    QPushButton *m_kexpButton = nullptr;
    QPushButton *m_playLandSongButton = nullptr;

    QSlider *m_seekSlider = nullptr;
    QLabel *m_timeLabel = nullptr;

    QSlider *m_volumeSlider = nullptr;

    QCheckBox *m_fftCheckBox = nullptr;

    QAction *m_gaplessAction = nullptr;

    QListWidget *m_playlistWidget = nullptr;
    QLabel *m_statusLabel = nullptr;

    QLCDNumber *m_sdlµsDiagLabel = nullptr;
    QLCDNumber *m_sdlDiagLabel = nullptr;
    QLCDNumber *m_decodeDiagLabel = nullptr;
    QLCDNumber *m_fftDiagLabel = nullptr;
    QLabel *m_sdlµsLabel = nullptr;
    QLabel *m_sdlLabel = nullptr;
    QLabel *m_decodeLabel = nullptr;
    QLabel *m_fftLabel = nullptr;

    bool m_seeking = false;
    int64_t m_durationMs = 0;
    DragonIcyMetadata m_lastIcyMetadata;
};
