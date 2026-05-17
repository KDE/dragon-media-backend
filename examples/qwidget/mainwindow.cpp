/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "mainwindow.h"
#include "dragonplaylist.h"
#include "dragonspectrogram.h"
#include "dragonvisualizer.h"

#include <dragonsdl/dragondiagnostics.h>
#include <dragonsdl/dragonfftframe.h>
#include <dragonsdl/dragonplayer.h>

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QLCDNumber>

#include <KLocalizedString>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <KIO/ListJob>
#include <KIO/UDSEntry>

using namespace Qt::StringLiterals;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_player = new DragonPlayer(this);
    m_playlist = new DragonPlaylist(m_player, this);
    m_visualizer = new DragonVisualizer(this);
    m_spectrogram = new DragonSpectrogram(this);
    setupUi();
    connectPlayer();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    setWindowTitle(i18n("Dragon SDL Media Player"));
    resize(700, 400);

    auto *fileMenu = menuBar()->addMenu(i18n("&File"));
    fileMenu->addAction(i18n("&Open File..."), QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addAction(i18n("Open &Multiple Files..."), this, &MainWindow::openMultipleFiles);
    fileMenu->addAction(i18n("Add &Network URL..."), this, &MainWindow::addNetworkUrl);
    fileMenu->addSeparator();
    fileMenu->addAction(i18n("&Clear Playlist"), this, &MainWindow::clearPlaylist);
    fileMenu->addSeparator();
    fileMenu->addAction(i18n("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    auto *playbackMenu = menuBar()->addMenu(i18n("&Playback"));
    m_gaplessAction = playbackMenu->addAction(i18n("&Gapless Playback"));
    m_gaplessAction->setCheckable(true);
    m_gaplessAction->setChecked(true);
    connect(m_gaplessAction, &QAction::toggled, m_playlist, &DragonPlaylist::setGaplessEnabled);

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
    m_prevButton->setToolTip(i18n("Previous"));
    m_playButton = new QPushButton(this);
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playButton->setToolTip(i18n("Play"));
    m_pauseButton = new QPushButton(this);
    m_pauseButton->setIcon(style()->standardIcon(QStyle::SP_MediaPause));
    m_pauseButton->setToolTip(i18n("Pause"));
    m_stopButton = new QPushButton(this);
    m_stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    m_stopButton->setToolTip(i18n("Stop"));
    m_nextButton = new QPushButton(this);
    m_nextButton->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    m_nextButton->setToolTip(i18n("Next"));

    hBtnLayout->addWidget(m_prevButton);
    hBtnLayout->addWidget(m_playButton);
    hBtnLayout->addWidget(m_pauseButton);
    hBtnLayout->addWidget(m_stopButton);
    hBtnLayout->addWidget(m_nextButton);
    hBtnLayout->addStretch();
    m_kexpButton = new QPushButton(i18n("Play KEXP"), this);
    m_kexpButton->setToolTip(i18n("Play KEXP Radio Stream"));
    hBtnLayout->addWidget(m_kexpButton);

    m_playCdButton = new QPushButton(i18n("Play CD"), this);
    m_playCdButton->setToolTip(i18n("Play Audio CD"));
    hBtnLayout->addWidget(m_playCdButton);

    vLayout->addLayout(hBtnLayout);

    auto *hSeekLayout = new QHBoxLayout();
    m_timeLabel = new QLabel(u"00:00 / 00:00"_s, this);
    m_timeLabel->setMinimumWidth(100);
    m_seekSlider = new QSlider(Qt::Horizontal, this);
    m_seekSlider->setRange(0, 0);
    m_seekSlider->installEventFilter(this);
    hSeekLayout->addWidget(m_timeLabel);
    hSeekLayout->addWidget(m_seekSlider, 1);
    vLayout->addLayout(hSeekLayout);

    auto *hVolLayout = new QHBoxLayout();
    auto *volLabel = new QLabel(i18n("Volume:"), this);
    volLabel->setMinimumWidth(60);
    m_volumeSlider = new QSlider(Qt::Horizontal, this);
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(100);
    m_volumeSlider->setMaximumWidth(200);
    hVolLayout->addWidget(volLabel);
    hVolLayout->addWidget(m_volumeSlider, 1);
    hVolLayout->addStretch();
    vLayout->addLayout(hVolLayout);

    auto *visualizerLabel = new QLabel(i18n("Visualizer"), this);
    visualizerLabel->setStyleSheet(u"font-weight: bold;"_s);
    vLayout->addWidget(visualizerLabel);
    m_visualizer->setFixedHeight(DragonVisualizer::PreferredHeight);
    vLayout->addWidget(m_visualizer);

    m_spectrogram->setFixedHeight(DragonSpectrogram::PreferredHeight);
    vLayout->addWidget(m_spectrogram);

    m_fftCheckBox = new QCheckBox(i18n("Enable FFT visualization"), this);
    vLayout->addWidget(m_fftCheckBox);

    vLayout->addStretch();
    mainLayout->addWidget(leftPanel, 2);

    auto *rightPanel = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    auto *playlistLabel = new QLabel(i18n("Playlist"), this);
    playlistLabel->setStyleSheet(u"font-weight: bold;"_s);
    rightLayout->addWidget(playlistLabel);

    m_playlistWidget = new QListWidget(this);
    m_playlistWidget->setMinimumWidth(200);
    rightLayout->addWidget(m_playlistWidget, 1);

    mainLayout->addWidget(rightPanel, 1);

    setCentralWidget(central);

    m_statusLabel = new QLabel(i18n("Ready"), this);
    statusBar()->addWidget(m_statusLabel, 1);

    m_sdlµsDiagLabel = new QLCDNumber(5, this);
    m_sdlµsDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_sdlµsDiagLabel->setMinimumWidth(60);
    m_sdlDiagLabel = new QLCDNumber(5, this);
    m_sdlDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_sdlDiagLabel->setMinimumWidth(60);
    m_decodeDiagLabel = new QLCDNumber(5, this);
    m_decodeDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_decodeDiagLabel->setMinimumWidth(60);
    m_fftDiagLabel = new QLCDNumber(5, this);
    m_fftDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_fftDiagLabel->setMinimumWidth(60);

    m_sdlµsLabel = new QLabel(i18n("SDL buffer µs:"), this);
    m_sdlLabel = new QLabel(i18n("Hz:"), this);
    m_decodeLabel = new QLabel(i18n("Decode:"), this);
    m_fftLabel = new QLabel(i18n("FFT:"), this);

    auto *diagContainer = new QWidget(this);
    auto *diagLayout = new QHBoxLayout(diagContainer);
    diagLayout->setContentsMargins(4, 0, 4, 0);
    diagLayout->setSpacing(8);
    diagLayout->addWidget(m_sdlµsLabel);
    diagLayout->addWidget(m_sdlµsDiagLabel);
    diagLayout->addWidget(m_sdlLabel);
    diagLayout->addWidget(m_sdlDiagLabel);
    diagLayout->addWidget(m_decodeLabel);
    diagLayout->addWidget(m_decodeDiagLabel);
    diagLayout->addWidget(m_fftLabel);
    diagLayout->addWidget(m_fftDiagLabel);
    statusBar()->addPermanentWidget(diagContainer);

    auto *diagnostics = new DragonDiagnostics(m_player);
    auto *diagTimer = new QTimer(this);
    connect(diagTimer, &QTimer::timeout, this, [this, diagnostics]() {
        const int µs = diagnostics->sdlAudioBufferUs();
        m_sdlµsDiagLabel->display(µs);

        const float callbackHz = diagnostics->audioCallbackHz();
        m_sdlDiagLabel->display(static_cast<int>(callbackHz));

        const std::size_t decodeSamples = diagnostics->decodeQueueSize();
        m_decodeDiagLabel->display(static_cast<int>(decodeSamples));

        const std::size_t fftSamples = diagnostics->fftQueueSize();
        m_fftDiagLabel->display(static_cast<int>(fftSamples));
    });
    diagTimer->start(500);
}

void MainWindow::connectPlayer()
{
    connect(m_playButton, &QPushButton::clicked, m_player, &DragonPlayer::play);
    connect(m_stopButton, &QPushButton::clicked, m_player, &DragonPlayer::stop);
    connect(m_pauseButton, &QPushButton::clicked, m_player, &DragonPlayer::pause);
    connect(m_nextButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playNext);
    connect(m_prevButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playPrevious);
    connect(m_kexpButton, &QPushButton::clicked, this, &MainWindow::playKexp);
    connect(m_playCdButton, &QPushButton::clicked, this, &MainWindow::playCd);

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

    connect(m_playlist, &DragonPlaylist::gaplessEnabledChanged, this, [this](bool enabled) {
        m_gaplessAction->setChecked(enabled);
    });

    connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() {
        m_seeking = true;
    });
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int position) {
        m_player->seek(static_cast<int64_t>(position));
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        m_seeking = false;
    });
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
            m_statusLabel->setText(i18n("Error: %1").arg(static_cast<int>(error)));
    });

    connect(m_player, &DragonPlayer::trackChanged, this, [this]() {
        m_statusLabel->setText(i18n("Playing (seamless transition)"));
    });

    connect(m_player, &DragonPlayer::fftFrameReady, this, &MainWindow::updateFftFrame);
    connect(m_fftCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        m_player->setFftMode(checked ? DragonPlayer::FftMode::Both : DragonPlayer::FftMode::Off);
    });

    connect(m_player, &DragonPlayer::currentPlayingForRadiosChanged, this, &MainWindow::updateIcyMetadata);
}

void MainWindow::openFile()
{
    QSettings settings;
    const QUrl lastDir = settings.value("lastOpenDirUrl"_L1, QUrl::fromLocalFile(QDir::homePath())).toUrl();

    const QUrl url =
        QFileDialog::getOpenFileUrl(this, i18n("Open Audio File"), lastDir, i18n("Audio Files (*.mp3 *.wav *.ogg *.flac *.aac *.m4a);;All Files (*)"));

    if (url.isValid() && !url.isEmpty()) {
        m_playlist->clear();
        m_playlist->addTrack(url);
        settings.setValue("lastOpenDirUrl"_L1, url.adjusted(QUrl::RemoveFilename));
    }
}

void MainWindow::openMultipleFiles()
{
    QSettings settings;
    const QString lastDir = settings.value("lastOpenDir"_L1, QDir::homePath()).toString();

    const QStringList files =
        QFileDialog::getOpenFileNames(this, i18n("Open Audio Files"), lastDir, i18n("Audio Files (*.mp3 *.wav *.ogg *.flac *.aac *.m4a);;All Files (*)"));

    if (!files.isEmpty()) {
        QList<QUrl> urls;
        for (const QString &f : files) {
            urls.append(QUrl::fromLocalFile(f));
        }
        m_playlist->addTracks(urls);
        settings.setValue("lastOpenDir"_L1, QFileInfo(files.first()).absolutePath());
    }
}

void MainWindow::clearPlaylist()
{
    m_playlist->clear();
}

void MainWindow::addNetworkUrl()
{
    QSettings settings;
    const QString lastUrl = settings.value("lastNetworkUrl"_L1, "https://"_L1).toString();

    bool ok = false;
    const QString url = QInputDialog::getText(this, i18n("Add Network URL"), i18n("Enter the URL of the network stream:"), QLineEdit::Normal, lastUrl, &ok);

    if (ok && !url.isEmpty()) {
        QUrl networkUrl(url);
        if (networkUrl.isValid()) {
            m_playlist->addTrack(networkUrl);
            settings.setValue("lastNetworkUrl"_L1, url);
        } else {
            m_statusLabel->setText(i18n("Invalid URL: %1").arg(url));
        }
    }
}

void MainWindow::playKexp()
{
    m_playlist->clear();
    m_playlist->addTrack(QUrl("https://kexp.streamguys1.com/kexp160.aac"_L1));
}

void MainWindow::playCd()
{
    m_playlist->clear();
    m_statusLabel->setText(i18n("Listing CD tracks..."));

    auto *job = KIO::listDir(QUrl(u"audiocd:/"_s), KIO::HideProgressInfo);
    connect(job, &KIO::ListJob::entries, this, [this](KIO::Job *, const KIO::UDSEntryList &entries) {
        QList<QUrl> urls;
        for (const auto &entry : entries) {
            const QString name = entry.stringValue(KIO::UDSEntry::UDS_NAME);
            if (name.endsWith(u".wav"_s)) {
                QUrl url(u"audiocd:/"_s);
                url.setPath(u"/"_s + name);
                urls.append(url);
            }
        }
        std::sort(urls.begin(), urls.end(), [](const QUrl &a, const QUrl &b) {
            return a.toString() < b.toString();
        });
        m_playlist->addTracks(urls);
    });
    connect(job, &KJob::result, this, [this](KJob *job) {
        if (job->error()) {
            m_statusLabel->setText(i18n("Error listing CD: %1").arg(job->errorString()));
        } else {
            m_statusLabel->setText(i18n("CD tracks loaded"));
            if (m_playlist->count() > 0) {
                m_playlist->setCurrentIndex(0);
                m_player->play();
            }
        }
    });
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
        if (m_lastIcyMetadata.hasStreamTitle()) {
            m_statusLabel->setText(i18n("Playing: %1").arg(m_lastIcyMetadata.streamTitle()));
        } else {
            m_statusLabel->setText(i18n("Playing"));
        }
        break;
    case DragonPlayer::PlaybackState::PausedState:
        if (m_lastIcyMetadata.hasStreamTitle()) {
            m_statusLabel->setText(i18n("Paused: %1").arg(m_lastIcyMetadata.streamTitle()));
        } else {
            m_statusLabel->setText(i18n("Paused"));
        }
        break;
    case DragonPlayer::PlaybackState::StoppedState:
        m_lastIcyMetadata.clear();
        m_statusLabel->setText(i18n("Stopped"));
        break;
    }
}

void MainWindow::updatePosition(int64_t positionMs)
{
    if (!m_seeking)
        m_seekSlider->setValue(static_cast<int>(positionMs));

    m_timeLabel->setText("%1 / %2"_L1.arg(formatTime(positionMs)).arg(formatTime(m_durationMs)));
}

void MainWindow::updateDuration(int64_t durationMs)
{
    m_durationMs = durationMs;
    m_seekSlider->setMaximum(static_cast<int>(durationMs));
    m_timeLabel->setText("%1 / %2"_L1.arg(formatTime(m_player->position())).arg(formatTime(durationMs)));
}

void MainWindow::setPositionFromSlider()
{
    m_seeking = false;
    m_player->seek(static_cast<int64_t>(m_seekSlider->value()));
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_seekSlider && event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QRect sliderRect = m_seekSlider->rect();
        const int mouseX = static_cast<int>(mouseEvent->position().x());
        const double ratio = static_cast<double>(mouseX) / sliderRect.width();
        const int min = m_seekSlider->minimum();
        const int max = m_seekSlider->maximum();
        const int targetValue = min + static_cast<int>(ratio * (max - min));
        m_player->seek(static_cast<int64_t>(targetValue));
        return false;
    }
    return QMainWindow::eventFilter(obj, event);
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
        m_statusLabel->setText(i18n("No media"));
        break;
    case DragonPlayer::MediaStatus::LoadingMedia:
        m_lastIcyMetadata.clear();
        m_statusLabel->setText(i18n("Loading..."));
        break;
    case DragonPlayer::MediaStatus::LoadedMedia:
        m_statusLabel->setText(i18n("Loaded"));
        break;
    case DragonPlayer::MediaStatus::BufferingMedia:
        m_statusLabel->setText(i18n("Buffering..."));
        break;
    case DragonPlayer::MediaStatus::StalledMedia:
        m_statusLabel->setText(i18n("Stalled"));
        break;
    case DragonPlayer::MediaStatus::BufferedMedia:
        m_statusLabel->setText(i18n("Buffered"));
        break;
    case DragonPlayer::MediaStatus::EndOfMedia:
        m_statusLabel->setText(i18n("Finished"));
        break;
    case DragonPlayer::MediaStatus::InvalidMedia:
        m_statusLabel->setText(i18n("Invalid media"));
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
        return "00:00"_L1;

    const int64_t totalSeconds = ms / 1000;
    const int64_t minutes = totalSeconds / 60;
    const int64_t seconds = totalSeconds % 60;
    const int64_t hours = minutes / 60;
    const int64_t mins = minutes % 60;

    if (hours > 0)
        return u"%1:%2:%3"_s.arg(hours).arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));

    return u"%1:%2"_s.arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

void MainWindow::updateFftFrame(const DragonFftFrame &frame)
{
    if (!m_fftCheckBox->isChecked()) {
        return;
    }
    m_visualizer->updateBarData(frame.barData);
    m_spectrogram->updateFrequencies(frame.frequenciesDb);
}

void MainWindow::updateIcyMetadata(const DragonIcyMetadata &metadata)
{
    m_lastIcyMetadata = metadata;
    if (metadata.hasStreamTitle()) {
        m_statusLabel->setText(i18n("Playing: %1").arg(metadata.streamTitle()));
    }
}
