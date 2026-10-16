/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "mainwindow.h"
#include "dragonplaylist.h"
#include "dragonspectrogram.h"
#include "dragonvisualizer.h"

#include "player/dragondiagnostics.h"
#include <DragonAudioOutput>
#include <DragonCdToc>
#include <DragonCdTocReader>
#include <DragonCdTrack>
#include <DragonFftFrame>
#include <DragonPlayer>
#include <DragonSpectrumAnalyzer>

#include <chrono>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>

#include <KCDDB/CDInfo>
#include <KCDDB/Client>
#include <KCDDB/KCDDB>
#include <QCoroSignal>
#include <QCoroTask>

using namespace Qt::StringLiterals;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_player = new DragonPlayer(this);
    m_playlist = new DragonPlaylist(m_player, this);
    m_visualizer = new DragonVisualizer(this);
    m_spectrogram = new DragonSpectrogram(this);
    m_spectrumAnalyzer = new DragonSpectrumAnalyzer(m_player, this);
    setupUi();
    connectPlayer();
}

MainWindow::~MainWindow()
{
    if (m_cdTocReader) {
        m_cdTocReader->deleteLater();
    }
    if (m_cddbThread) {
        m_cddbThread->quit();
        m_cddbThread->wait(2000);
    }
}

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

    m_playLandSongButton = new QPushButton(i18n("Play The Land Song"), this);
    m_playLandSongButton->setToolTip(i18n("Play The Land Song"));
    hBtnLayout->addWidget(m_playLandSongButton);

    m_playCdButton = new QPushButton(i18n("Play CD"), this);
    m_playCdButton->setToolTip(i18n("Play all tracks of the audio CD in the disc drive"));
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

    auto *sinkLabel = new QLabel(i18n("Audio Sink"), this);
    sinkLabel->setStyleSheet(u"font-weight: bold;"_s);
    rightLayout->addWidget(sinkLabel);

    m_sinkComboBox = new QComboBox(this);
    m_sinkComboBox->addItem(i18n("Auto"), static_cast<int>(DragonAudioOutput::Backend::Auto));
    m_sinkComboBox->addItem(i18n("PipeWire"), static_cast<int>(DragonAudioOutput::Backend::PipeWire));
    m_sinkComboBox->addItem(i18n("PulseAudio"), static_cast<int>(DragonAudioOutput::Backend::PulseAudio));
    m_sinkComboBox->addItem(i18n("SDL"), static_cast<int>(DragonAudioOutput::Backend::SDL));
    m_sinkComboBox->addItem(i18n("Null"), static_cast<int>(DragonAudioOutput::Backend::Null));
    int defaultIdx = m_sinkComboBox->findData(static_cast<int>(DragonAudioOutput::Backend::Auto));
    if (defaultIdx >= 0)
        m_sinkComboBox->setCurrentIndex(defaultIdx);

    // We do not add the combo box to m_playerConnections, because the combo box outlives the player.
    connect(m_sinkComboBox, &QComboBox::activated, this, &MainWindow::changeAudioSink);
    rightLayout->addWidget(m_sinkComboBox);

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
    m_decodeDiagLabel = new QLCDNumber(5, this);
    m_decodeDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_decodeDiagLabel->setMinimumWidth(60);
    m_fftDiagLabel = new QLCDNumber(5, this);
    m_fftDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_fftDiagLabel->setMinimumWidth(60);
    m_underrunDiagLabel = new QLCDNumber(5, this);
    m_underrunDiagLabel->setSegmentStyle(QLCDNumber::Flat);
    m_underrunDiagLabel->setMinimumWidth(60);

    m_sdlµsLabel = new QLabel(i18n("Buffer µs:"), this);
    m_decodeLabel = new QLabel(i18n("Decode:"), this);
    m_fftLabel = new QLabel(i18n("FFT:"), this);
    m_underrunLabel = new QLabel(i18n("Underruns:"), this);

    auto *diagContainer = new QWidget(this);
    auto *diagLayout = new QHBoxLayout(diagContainer);
    diagLayout->setContentsMargins(4, 0, 4, 0);
    diagLayout->setSpacing(8);
    diagLayout->addWidget(m_sdlµsLabel);
    diagLayout->addWidget(m_sdlµsDiagLabel);
    diagLayout->addWidget(m_decodeLabel);
    diagLayout->addWidget(m_decodeDiagLabel);
    diagLayout->addWidget(m_fftLabel);
    diagLayout->addWidget(m_fftDiagLabel);
    diagLayout->addWidget(m_underrunLabel);
    diagLayout->addWidget(m_underrunDiagLabel);
    statusBar()->addPermanentWidget(diagContainer);

    m_diagnostics = new DragonDiagnostics(m_player);
    m_diagnostics->setParent(this);
    m_diagnostics->setSpectrumAnalyzer(m_spectrumAnalyzer);
    auto *diagTimer = new QTimer(this);
    connect(diagTimer, &QTimer::timeout, this, [this]() {
        if (!m_diagnostics)
            return;
        const auto bufferUs = m_diagnostics->audioBufferDuration();
        m_sdlµsDiagLabel->display(bufferUs ? static_cast<int>(bufferUs->count()) : -1);

        const std::size_t decodeSamples = m_diagnostics->decodeQueueSize();
        m_decodeDiagLabel->display(static_cast<int>(decodeSamples));

        const std::size_t fftSamples = m_diagnostics->fftQueueSize();
        m_fftDiagLabel->display(static_cast<int>(fftSamples));

        const int underruns = m_diagnostics->audioUnderrunCount();
        m_underrunDiagLabel->display(underruns);
    });
    diagTimer->start(50);
}

void MainWindow::connectPlayer()
{
    for (auto c : m_playerConnections) {
        QObject::disconnect(c);
    }
    m_playerConnections.clear();

    m_playerConnections << connect(m_playButton, &QPushButton::clicked, m_player, &DragonPlayer::play);
    m_playerConnections << connect(m_stopButton, &QPushButton::clicked, m_player, &DragonPlayer::stop);
    m_playerConnections << connect(m_pauseButton, &QPushButton::clicked, m_player, &DragonPlayer::pause);
    m_playerConnections << connect(m_nextButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playNext);
    m_playerConnections << connect(m_prevButton, &QPushButton::clicked, m_playlist, &DragonPlaylist::playPrevious);
    m_playerConnections << connect(m_kexpButton, &QPushButton::clicked, this, &MainWindow::playKexp);
    m_playerConnections << connect(m_playLandSongButton, &QPushButton::clicked, this, &MainWindow::playLandSong);
    m_playerConnections << connect(m_playCdButton, &QPushButton::clicked, this, &MainWindow::playCd);

    m_playerConnections << connect(m_playlistWidget, &QListWidget::activated, this, [this](const QModelIndex &index) {
        m_playlist->setCurrentIndex(index.row());
    });

    m_playerConnections << connect(m_playlist, &DragonPlaylist::tracksChanged, this, [this]() {
        m_playlistWidget->clear();
        for (int i = 0; i < m_playlist->count(); ++i) {
            m_playlistWidget->addItem(entryLabel(i));
        }
    });

    m_playerConnections << connect(m_playlist, &DragonPlaylist::entryMetadataChanged, this, &MainWindow::updateEntryMetadata);

    m_playerConnections << connect(m_playlist, &DragonPlaylist::currentIndexChanged, this, &MainWindow::updatePlaylistCurrentIndex);

    m_playerConnections << connect(m_gaplessAction, &QAction::toggled, m_playlist, &DragonPlaylist::setGaplessEnabled);

    m_playerConnections << connect(m_playlist, &DragonPlaylist::gaplessEnabledChanged, this, [this](bool enabled) {
        m_gaplessAction->setChecked(enabled);
    });

    m_playerConnections << connect(m_seekSlider, &QSlider::sliderPressed, this, [this]() {
        m_seeking = true;
    });
    m_playerConnections << connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int position) {
        m_player->setPosition(std::chrono::milliseconds{position});
    });
    m_playerConnections << connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        m_seeking = false;
    });
    m_playerConnections << connect(m_player, &DragonPlayer::positionChanged, this, &MainWindow::updatePosition);
    m_playerConnections << connect(m_player, &DragonPlayer::durationChanged, this, [this](std::optional<std::chrono::milliseconds> duration) {
        if (duration) {
            updateDuration(*duration);
        }
    });

    m_playerConnections << connect(m_volumeSlider, &QSlider::valueChanged, this, &MainWindow::setVolumeFromSlider);
    m_playerConnections << connect(m_player->audioOutput(), &DragonAudioOutput::volumeChanged, this, [this]() {
        const int vol = static_cast<int>(m_player->audioOutput()->volume() * 100.0f);
        if (m_volumeSlider->value() != vol)
            m_volumeSlider->setValue(vol);
    });

    m_playerConnections << connect(m_player, &DragonPlayer::playbackStateChanged, this, [this](DragonPlayer::PlaybackState, DragonPlayer::PlaybackState) {
        updatePlaybackState();
    });
    m_playerConnections << connect(m_player, &DragonPlayer::statusChanged, this, &MainWindow::updateStatus);
    m_playerConnections << connect(m_player, &DragonPlayer::errorChanged, this, [this](DragonPlayer::Error error) {
        if (error != DragonPlayer::Error::NoError)
            m_statusLabel->setText(i18n("Error: %1").arg(static_cast<int>(error)));
    });

    m_playerConnections << connect(m_player, &DragonPlayer::trackChanged, this, [this]() {
        m_statusLabel->setText(i18n("Playing (seamless transition)"));
    });

    m_playerConnections << connect(m_spectrumAnalyzer, &DragonSpectrumAnalyzer::frameReady, this, &MainWindow::updateFftFrame);
    m_playerConnections << connect(m_fftCheckBox, &QCheckBox::toggled, this, [this](bool checked) {
        m_spectrumAnalyzer->setMode(checked ? DragonSpectrumAnalyzer::Mode::Both : DragonSpectrumAnalyzer::Mode::Off);
    });

    m_playerConnections << connect(m_player, &DragonPlayer::currentPlayingForRadiosChanged, this, &MainWindow::updateIcyMetadata);
}

void MainWindow::changeAudioSink(int index)
{
    auto sink = static_cast<DragonAudioOutput::Backend>(m_sinkComboBox->itemData(index).toInt());

    QList<QUrl> currentTracks = m_playlist->tracks();
    int currentIndex = m_playlist->currentIndex();
    qint64 currentPosition = 0;
    if (const auto pos = m_player->position(); pos >= std::chrono::milliseconds{0}) {
        currentPosition = pos.count();
    }
    bool isPlaying = m_player->playbackState() == DragonPlayer::PlaybackState::PlayingState;
    qreal currentVolume = m_player->audioOutput()->volume();
    bool gaplessEnabled = m_playlist->gaplessEnabled();
    auto fftMode = m_spectrumAnalyzer->mode();
    auto fftRate = m_spectrumAnalyzer->frameRate();

    delete m_playlist;
    m_playlist = nullptr;
    delete m_diagnostics;
    m_diagnostics = nullptr;
    delete m_spectrumAnalyzer;
    m_spectrumAnalyzer = nullptr;
    delete m_player;
    m_player = nullptr;

    m_player = new DragonPlayer(sink, this);
    m_playlist = new DragonPlaylist(m_player, this);
    m_diagnostics = new DragonDiagnostics(m_player);
    m_diagnostics->setParent(this);
    m_spectrumAnalyzer = new DragonSpectrumAnalyzer(m_player, this);
    m_diagnostics->setSpectrumAnalyzer(m_spectrumAnalyzer);

    m_playlist->setGaplessEnabled(gaplessEnabled);
    m_player->audioOutput()->setVolume(currentVolume);
    m_spectrumAnalyzer->setMode(fftMode);
    m_spectrumAnalyzer->setFrameRate(fftRate);

    connectPlayer();

    if (!currentTracks.isEmpty()) {
        m_playlist->addTracks(currentTracks);
        m_playlist->setCurrentIndex(currentIndex);
        m_player->setPosition(std::chrono::milliseconds{currentPosition});
        if (isPlaying) {
            m_player->play();
        }
    }
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

void MainWindow::playLandSong()
{
    m_playlist->clear();
    m_playlist->addTrack(QUrl("http://www.historyworkshop.org.uk/wp-content/uploads/2011/05/01-The-Land-Song.mp3"_L1));
    m_playlist->setCurrentIndex(0);
    m_player->play();
}

void MainWindow::playCd()
{
    if (m_cdTocReader) {
        return;
    }

    m_playCdButton->setEnabled(false);
    m_statusLabel->setText(i18n("Reading the disc…"));

    m_cdTocReader = new DragonCdTocReader(this);
    connect(m_cdTocReader, &DragonCdTocReader::tocReady, this, [this](const DragonCdToc &toc) {
        m_cdTocReader->deleteLater();
        m_cdTocReader = nullptr;
        m_playCdButton->setEnabled(true);

        // Playback starts from purely local TOC information, before any
        // network activity: placeholder titles plus each track's URL and
        // duration.
        QList<DragonPlaylist::Entry> entries;
        m_cdTrackNumberByUrl.clear();
        for (const DragonCdTrack &track : toc.tracks()) {
            if (!track.isAudio()) {
                continue;
            }
            DragonPlaylist::Entry entry;
            entry.url = track.url();
            entry.title = i18n("Track %1", track.number());
            entry.duration = track.duration();
            entries.append(entry);
            m_cdTrackNumberByUrl.insert(track.url(), track.number());
        }
        if (entries.isEmpty()) {
            m_statusLabel->setText(i18n("No audio tracks found on the CD."));
            return;
        }

        m_playlist->clear();
        m_playlist->addEntries(entries);
        m_statusLabel->setText(i18np("Playing %1 CD track…", "Playing %1 CD tracks…", entries.size()));

        // The metadata lookup runs concurrently with the playback that
        // has already started.
        startCddbLookup(toc);
    });
    connect(m_cdTocReader, &DragonCdTocReader::failed, this, [this](const QString &errorMessage) {
        m_cdTocReader->deleteLater();
        m_cdTocReader = nullptr;
        m_playCdButton->setEnabled(true);
        m_statusLabel->setText(errorMessage);
    });
    m_cdTocReader->requestToc();
}

// The metadata the CDDB lookup produces, keyed by CD track number; the
// playlist entries are matched back through the DragonCdTrack data they
// were built from.
struct CddbTitles {
    bool success = false;
    QMap<int, QString> trackTitles;
    QMap<int, QString> trackArtists;
};

Q_DECLARE_METATYPE(CddbTitles)

namespace
{

// KCDDB::Client::lookup() is a blocking network call; this worker runs
// it on its own thread and delivers the result through a queued signal.
class CddbLookupWorker : public QObject
{
    Q_OBJECT

public:
    explicit CddbLookupWorker(KCDDB::TrackOffsetList offsets)
        : m_offsets(std::move(offsets))
    {
    }

public Q_SLOTS:
    void start()
    {
        CddbTitles titles;

        KCDDB::Client client;
        const KCDDB::Result result = client.lookup(m_offsets);
        if (result == KCDDB::Success) {
            const QList<KCDDB::CDInfo> response = client.lookupResponse();
            if (!response.isEmpty()) {
                // The first entry is the best match, like the audiocd
                // KIO worker takes it.
                const KCDDB::CDInfo &info = response.first();
                titles.success = true;
                const int trackCount = info.numberOfTracks();
                for (int i = 0; i < trackCount; ++i) {
                    const KCDDB::TrackInfo track = info.track(i);
                    titles.trackTitles.insert(i + 1, track.get(KCDDB::Title).toString());
                    titles.trackArtists.insert(i + 1, track.get(KCDDB::Artist).toString());
                }
            }
        }

        Q_EMIT finished(titles);
        deleteLater();
    }

Q_SIGNALS:
    void finished(const CddbTitles &titles);

private:
    KCDDB::TrackOffsetList m_offsets;
};

}

QCoro::Task<void> MainWindow::runCddbLookup(const QList<quint32> &offsets)
{
    qRegisterMetaType<CddbTitles>();

    auto *worker = new CddbLookupWorker(offsets);
    m_cddbThread = new QThread(this);
    worker->moveToThread(m_cddbThread);
    connect(worker, &CddbLookupWorker::finished, m_cddbThread, &QThread::quit);
    m_cddbThread->start();

    QMetaObject::invokeMethod(worker, &CddbLookupWorker::start, Qt::QueuedConnection);
    const CddbTitles titles = co_await qCoro(worker, &CddbLookupWorker::finished);

    if (!titles.success) {
        // Nothing about playback is affected; the placeholder titles stay.
        m_statusLabel->setText(i18n("Playing (CDDB lookup failed)"));
        co_return;
    }

    // Deliberately crude: match each playlist entry to a CDDB track by
    // the DragonCdTrack it was built from (its URL maps back to its
    // number), then rewrite the entry's title and artist in place.
    for (int i = 0; i < m_playlist->count(); ++i) {
        const QUrl url = m_playlist->trackAt(i);
        const int trackNumber = m_cdTrackNumberByUrl.value(url, -1);
        if (trackNumber < 0 || !titles.trackTitles.contains(trackNumber)) {
            continue;
        }
        QString title = titles.trackTitles.value(trackNumber);
        if (title.isEmpty()) {
            title = i18n("Track %1", trackNumber);
        }
        m_playlist->setEntryMetadata(i, title, titles.trackArtists.value(trackNumber));
    }
    m_statusLabel->setText(i18n("Playing (titles updated from CDDB)"));
}

void MainWindow::startCddbLookup(const DragonCdToc &toc)
{
    m_cddbTask = runCddbLookup(toc.cddbTrackOffsetList());
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
        if (!m_lastIcyMetadata.streamTitle().isEmpty()) {
            m_statusLabel->setText(i18n("Playing: %1").arg(m_lastIcyMetadata.streamTitle()));
        } else {
            m_statusLabel->setText(i18n("Playing"));
        }
        break;
    case DragonPlayer::PlaybackState::PausedState:
        if (!m_lastIcyMetadata.streamTitle().isEmpty()) {
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

void MainWindow::updatePosition(std::chrono::milliseconds position)
{
    if (!m_seeking)
        m_seekSlider->setValue(static_cast<int>(position.count()));

    m_timeLabel->setText("%1 / %2"_L1.arg(formatTime(position)).arg(formatTime(m_duration)));
}

void MainWindow::updateDuration(std::chrono::milliseconds duration)
{
    m_duration = duration;
    m_seekSlider->setMaximum(static_cast<int>(duration.count()));
    m_timeLabel->setText("%1 / %2"_L1.arg(formatTime(m_player->position())).arg(formatTime(duration)));
}

void MainWindow::setPositionFromSlider()
{
    m_seeking = false;
    m_player->setPosition(std::chrono::milliseconds{m_seekSlider->value()});
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
        m_player->setPosition(std::chrono::milliseconds{targetValue});
        return false;
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::setVolumeFromSlider(int value)
{
    m_player->audioOutput()->setVolume(value / 100.0);
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

void MainWindow::updateEntryMetadata(int index)
{
    if (auto *item = m_playlistWidget->item(index)) {
        item->setText(entryLabel(index));
    }
}

QString MainWindow::entryLabel(int index) const
{
    const DragonPlaylist::Entry entry = m_playlist->entryAt(index);

    QString label = u"%1. "_s.arg(index + 1, 2, 10, QLatin1Char(u'0'));
    if (!entry.artist.isEmpty()) {
        label += entry.artist + u" - "_s;
    }
    label += entry.title.isEmpty() ? entry.url.toString() : entry.title;
    if (entry.duration > std::chrono::milliseconds{0}) {
        label += u" ("_s + formatTime(entry.duration) + u')';
    }
    return label;
}

QString MainWindow::formatTime(std::chrono::milliseconds position)
{
    if (position < std::chrono::milliseconds{0})
        return "00:00"_L1;

    const qint64 ms = position.count();
    const qint64 totalSeconds = ms / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    const qint64 hours = minutes / 60;
    const qint64 mins = minutes % 60;

    if (hours > 0)
        return u"%1:%2:%3"_s.arg(hours).arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));

    return u"%1:%2"_s.arg(mins, 2, 10, QLatin1Char('0')).arg(seconds, 2, 10, QLatin1Char('0'));
}

void MainWindow::updateFftFrame(const DragonFftFrame &frame)
{
    if (!m_fftCheckBox->isChecked()) {
        return;
    }
    m_visualizer->updateBarData(frame.bars());
    m_spectrogram->updateFrequencies(frame.frequencies());
}

void MainWindow::updateIcyMetadata(const DragonIcyMetadata &metadata)
{
    m_lastIcyMetadata = metadata;
    if (!metadata.streamTitle().isEmpty()) {
        m_statusLabel->setText(i18n("Playing: %1").arg(metadata.streamTitle()));
    }
}

#include "mainwindow.moc"
