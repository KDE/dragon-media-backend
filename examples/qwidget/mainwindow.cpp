/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "mainwindow.h"
#include "dragonplaylist.h"

#include <dragonsdl/dragonplayer.h>

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QPushButton>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_player = new DragonPlayer(this);
    m_playlist = new DragonPlaylist(m_player, this);
    setupUi();
    connectPlayer();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    setWindowTitle(tr("Dragon SDL Media Player"));
    resize(700, 400);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&Open File..."), QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addAction(tr("Open &Multiple Files..."), this, &MainWindow::openMultipleFiles);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Clear Playlist"), this, &MainWindow::clearPlaylist);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto *central = new QWidget(this);
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    auto *leftPanel = new QWidget(this);
    auto *vLayout = new QVBoxLayout(leftPanel);
    vLayout->setSpacing(12);

    auto *hBtnLayout = new QHBoxLayout();
    m_prevButton = new QPushButton(this);
    m_prevButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    m_prevButton->setToolTip(tr("Previous"));
    m_playButton = new QPushButton(this);
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playButton->setToolTip(tr("Play"));
    m_pauseButton = new QPushButton(this);
    m_pauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    m_pauseButton->setToolTip(tr("Pause"));
    m_stopButton = new QPushButton(this);
    m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_stopButton->setToolTip(tr("Stop"));
    m_nextButton = new QPushButton(this);
    m_nextButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_nextButton->setToolTip(tr("Next"));

    hBtnLayout->addWidget(m_prevButton);
    hBtnLayout->addWidget(m_playButton);
    hBtnLayout->addWidget(m_pauseButton);
    hBtnLayout->addWidget(m_stopButton);
    hBtnLayout->addWidget(m_nextButton);
    hBtnLayout->addStretch();
    vLayout->addLayout(hBtnLayout);

    auto *hSeekLayout = new QHBoxLayout();
    m_timeLabel = new QLabel("00:00 / 00:00", this);
    m_timeLabel->setMinimumWidth(100);
    m_seekSlider = new QSlider(Qt::Horizontal, this);
    m_seekSlider->setRange(0, 0);
    hSeekLayout->addWidget(m_timeLabel);
    hSeekLayout->addWidget(m_seekSlider, 1);
    vLayout->addLayout(hSeekLayout);

    auto *hVolLayout = new QHBoxLayout();
    auto *volLabel = new QLabel(tr("Volume:"), this);
    volLabel->setMinimumWidth(60);
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setMaximumWidth(200);
    hVolLayout->addWidget(volLabel);
    hVolLayout->addWidget(m_volumeSlider, 1);
    hVolLayout->addStretch();
    vLayout->addLayout(hVolLayout);

    vLayout->addStretch();
    mainLayout->addWidget(leftPanel, 2);

    auto *rightPanel = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    auto *playlistLabel = new QLabel(tr("Playlist"), this);
    playlistLabel->setStyleSheet("font-weight: bold;");
    rightLayout->addWidget(playlistLabel);

    m_playlistWidget = new QListWidget(this);
    m_playlistWidget->setMinimumWidth(200);
    rightLayout->addWidget(m_playlistWidget, 1);

    mainLayout->addWidget(rightPanel, 1);

    setCentralWidget(central);

    m_statusLabel = new QLabel(tr("Ready"), this);
    statusBar()->addWidget(m_statusLabel);
}

void MainWindow::connectPlayer()
{
    connect(m_playButton, &QPushButton::clicked, m_player, &DragonPlayer::play);
    connect(m_stopButton, &QPushButton::clicked, m_player, &DragonPlayer::stop);
    connect(m_pauseButton, &QPushButton::clicked, m_player, &DragonPlayer::pause);
    connect(m_nextButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playNext);
    connect(m_prevButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playPrevious);

    connect(m_playlistWidget, &QListWidget::activated, this, [this](const QModelIndex &index) {
        m_playlist->setCurrentIndex(index.row());
    });

    connect(m_playlist, &DragonPlaylist::tracksChanged, this, [this]() {
        m_playlistWidget->clear();
        for (const QUrl &url : m_playlist->tracks()) {
            QString name = url.fileName();
            if (name.isEmpty()) {
                name = url.toString();
            }
            m_playlistWidget->addItem(name);
        }
    });

    connect(m_playlist, &DragonPlaylist::currentIndexChanged, this, &MainWindow::updatePlaylistCurrentIndex);

    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() {
        m_seeking = true;
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, &MainWindow::setPositionFromSlider);
    connect(m_player, &DragonPlayer::positionChanged, this, &MainWindow::updatePosition);
    connect(m_player, &DragonPlayer::durationChanged, this, &MainWindow::updateDuration);

    connect(m_volumeSlider, &QSlider::valueChanged, this, &MainWindow::setVolumeFromSlider);
    connect(m_player, &DragonPlayer::volumeChanged, this, [this]() {
        const int vol = static_cast<int>(m_player->volume() * 100.0f);
        if (m_volumeSlider->value() != vol)
            m_volumeSlider->setValue(vol);
    });

    connect(m_player, &DragonPlayer::playbackStateChanged, this, &MainWindow::updatePlaybackState);
    connect(m_player, &DragonPlayer::statusChanged, this, &MainWindow::updateStatus);
    connect(m_player, &DragonPlayer::errorChanged, this, [this](DragonPlayer::Error error) {
        if (error != DragonPlayer::Error::NoError)
            m_statusLabel->setText(tr("Error: %1").arg(static_cast<int>(error)));
    });

    connect(m_player, &DragonPlayer::trackChanged, this, [this]() {
        m_statusLabel->setText(tr("Playing (seamless transition)"));
    });
}

void MainWindow::openFile()
{
    const QString file =
        QFileDialog::getOpenFileName(this, tr("Open Audio File"), QDir::homePath(), tr("Audio Files (*.mp3 *.wav *.ogg *.flac *.aac *.m4a);;All Files (*)"));

    if (!file.isEmpty()) {
        m_playlist->clear();
        m_playlist->addTrack(QUrl::fromLocalFile(file));
    }
}

void MainWindow::openMultipleFiles()
{
    const QStringList files =
        QFileDialog::getOpenFileNames(this, tr("Open Audio Files"), QDir::homePath(), tr("Audio Files (*.mp3 *.wav *.ogg *.flac *.aac *.m4a);;All Files (*)"));

    if (!files.isEmpty()) {
        QList<QUrl> urls;
        for (const QString &f : files) {
            urls.append(QUrl::fromLocalFile(f));
        }
        m_playlist->addTracks(urls);
    }
}

void MainWindow::clearPlaylist()
{
    m_playlist->clear();
}

void MainWindow::playPlaylistItem(int index)
{
    m_playlist->setCurrentIndex(index);
}

void MainWindow::updatePlaybackState()
{
    const auto state = m_player->playbackState();

    m_playButton->setEnabled(state != DragonPlayer::PlaybackState::PlayingState);
    m_pauseButton->setEnabled(state == DragonPlayer::PlaybackState::PlayingState);
    m_stopButton->setEnabled(state != DragonPlayer::PlaybackState::StoppedState);
    m_nextButton->setEnabled(m_playlist->currentIndex() < m_playlist->count() - 1);
    m_prevButton->setEnabled(m_playlist->currentIndex() > 0);

    switch (state) {
    case DragonPlayer::PlaybackState::PlayingState:
        m_statusLabel->setText(tr("Playing"));
        break;
    case DragonPlayer::PlaybackState::PausedState:
        m_statusLabel->setText(tr("Paused"));
        break;
    case DragonPlayer::PlaybackState::StoppedState:
        m_statusLabel->setText(tr("Stopped"));
        break;
    }
}

void MainWindow::updatePosition(int64_t positionMs)
{
    if (!m_seeking)
        m_seekSlider->setValue(static_cast<int>(positionMs));

    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(formatTime(positionMs)).arg(formatTime(m_durationMs)));
}

void MainWindow::updateDuration(int64_t durationMs)
{
    m_durationMs = durationMs;
    m_seekSlider->setMaximum(static_cast<int>(durationMs));
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(formatTime(m_player->position())).arg(formatTime(durationMs)));
}

void MainWindow::setPositionFromSlider()
{
    m_seeking = false;
    m_player->seek(static_cast<int64_t>(m_seekSlider->value()));
}

void MainWindow::setVolumeFromSlider(int value)
{
    m_player->setVolume(static_cast<float>(value) / 100.0f);
}

void MainWindow::updateStatus()
{
    const auto status = m_player->status();
    switch (status) {
    case DragonPlayer::MediaStatus::NoMedia:
        m_statusLabel->setText(tr("No media"));
        break;
    case DragonPlayer::MediaStatus::LoadingMedia:
        m_statusLabel->setText(tr("Loading..."));
        break;
    case DragonPlayer::MediaStatus::LoadedMedia:
        m_statusLabel->setText(tr("Loaded"));
        break;
    case DragonPlayer::MediaStatus::BufferingMedia:
        m_statusLabel->setText(tr("Buffering..."));
        break;
    case DragonPlayer::MediaStatus::StalledMedia:
        m_statusLabel->setText(tr("Stalled"));
        break;
    case DragonPlayer::MediaStatus::BufferedMedia:
        m_statusLabel->setText(tr("Buffered"));
        break;
    case DragonPlayer::MediaStatus::EndOfMedia:
        m_statusLabel->setText(tr("Finished"));
        break;
    case DragonPlayer::MediaStatus::InvalidMedia:
        m_statusLabel->setText(tr("Invalid media"));
        break;
    }
}

void MainWindow::updatePlaylistCurrentIndex(int index)
{
    for (int i = 0; i < m_playlistWidget->count(); ++i) {
        auto *item = m_playlistWidget->item(i);
        if (i == index) {
            item->setBackground(palette().highlight());
            item->setForeground(palette().highlightedText());
        } else {
            item->setBackground(palette().base());
            item->setForeground(palette().text());
        }
    }

    m_nextButton->setEnabled(index < m_playlist->count() - 1);
    m_prevButton->setEnabled(index > 0);
}

QString MainWindow::formatTime(int64_t ms)
{
    if (ms < 0)
        return QStringLiteral("00:00");

    const int64_t totalSeconds = ms / 1000;
    const int64_t minutes = totalSeconds / 60;
    const int64_t seconds = totalSeconds % 60;
    const int64_t hours = minutes / 60;
    const int64_t mins = minutes % 60;

    if (hours > 0)
        return QStringLiteral("%1:%2:%3").arg(hours).arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));

    return QStringLiteral("%1:%2").arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}
