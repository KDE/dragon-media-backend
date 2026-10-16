/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests for DragonCdStream and audiocd:/ playback. Most tests run against
 * generated bin/cue CD-DA images read through libcdio's bincue driver, so no
 * CD hardware is required.
 */

#include <QtCore>
#include <QtTest>

#include "helpers/cdimage.h"
#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "stream/cd/dragoncddisc.h"
#include "stream/cd/dragoncdstream.h"
#include "stream/cd/dragoncdurl.h"
#include "stream/dragonkiostream.h"
#include "stream/dragonstreamfactory.h"

#include <DragonAudioOutput>
#include <DragonPlayer>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>

#include <barrier>
#include <stop_token>
#include <thread>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using CdImage = CdImageTestUtil::CdImage;
using CdImageTestUtil::generateSectorPcm;

namespace
{

QUrl cdTrackUrl(const QString &fileName, const QString &device, const QString &extraQueryItem = {})
{
    QUrl url;
    url.setScheme(u"audiocd"_s);
    url.setPath(u'/' + fileName);
    QUrlQuery query;
    query.addQueryItem(u"device"_s, device);
    if (!extraQueryItem.isEmpty()) {
        query.addQueryItem(u"track"_s, extraQueryItem);
    }
    url.setQuery(query);
    return url;
}

std::vector<uint8_t> readAllFromStream(DragonCdStream &stream, qint64 limit)
{
    std::vector<uint8_t> result;
    std::vector<uint8_t> buffer(64 * 1024);
    std::stop_source stopSource;
    while (qint64(result.size()) < limit) {
        const int n = stream.read(buffer, stopSource.get_token());
        if (n < 0) {
            return {};
        }
        if (n == 0) {
            break;
        }
        result.insert(result.end(), buffer.begin(), buffer.begin() + n);
    }
    return result;
}

}

class TestCdStream : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testWavHeader();
    void testTrackNumberFromName();
    void testStartGeneratesWavStream();
    void testSequentialReadsAndEof();
    void testSeek();
    void testSmallAndLargeReadsAreConsistent();
    void testStopCancelsReads();
    void testErrorSlotCanStopStream();
    void testDirectoryUrlFails();
    void testMissingDeviceFails();
    void testDataTrackFails();
    void testDataOnlyDiscFails();
    void testTrackQueryItemOverride();
    void testFactoryRouting();
    void testStreamsShareDisc();
    void testConcurrentAcquireOpensDiscOnce();
    void testReadErrorInvalidatesDisc();
    void testChangedImageReopensDisc();
    void testGaplessPlayerTransition();
    void testLateNextSourceGaplessTransition();
    void testGaplessCdTrackFollowedByLocalTrackPreservesSeekable();

private:
    QString m_cuePath;
    QByteArray m_track1Pcm;
    int m_sectorsPerTrack = 0;
    std::unique_ptr<CdImage> m_image;

    void createTwoTrackImage();
};

void TestCdStream::testWavHeader()
{
    const auto header = DragonCdStream::makeWavHeader(300 * 2352);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(header.data()), 4), QByteArray("RIFF"));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(header.data() + 8), 4), QByteArray("WAVE"));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(header.data() + 12), 4), QByteArray("fmt "));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(header.data() + 36), 4), QByteArray("data"));

    const uint32_t riffSize = uint32_t(header[7]) << 24 | uint32_t(header[6]) << 16 | uint32_t(header[5]) << 8 | uint32_t(header[4]);
    QCOMPARE(riffSize, uint32_t(36 + 300 * 2352));

    const uint32_t dataSize = uint32_t(header[43]) << 24 | uint32_t(header[42]) << 16 | uint32_t(header[41]) << 8 | uint32_t(header[40]);
    QCOMPARE(dataSize, uint32_t(300 * 2352));

    const uint32_t sampleRate = uint32_t(header[27]) << 24 | uint32_t(header[26]) << 16 | uint32_t(header[25]) << 8 | uint32_t(header[24]);
    QCOMPARE(sampleRate, uint32_t(44100));
    const uint16_t channels = uint16_t(header[23]) << 8 | uint16_t(header[22]);
    QCOMPARE(channels, uint16_t(2));
    const uint16_t bits = uint16_t(header[35]) << 8 | uint16_t(header[34]);
    QCOMPARE(bits, uint16_t(16));
}

void TestCdStream::testTrackNumberFromName()
{
    const int first = 1;
    const int last = 15;

    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Track 01.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Track 1.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Track 1.cda"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Artist - 03 - Title.flac"_s, first, last).value_or(-1), 3);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"U2 - 01 - Beautiful Day.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"16_-_Some_Title.ogg"_s, first, last).value_or(-1), -1); // out of range
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"NoDigitsHere.wav"_s, first, last).value_or(-1), -1);
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Track 99.wav"_s, first, last).value_or(-1), -1);
    // First valid run wins when there is no zero-padded two-digit run.
    QCOMPARE(DragonCdUrl::trackNumberFromName(u"Track 3.wav"_s, first, last).value_or(-1), 3);
}

void TestCdStream::createTwoTrackImage()
{
    if (!m_cuePath.isEmpty()) {
        return;
    }
    m_image = std::make_unique<CdImage>();
    m_track1Pcm = m_image->addAudioTrack();
    m_image->addAudioTrack();
    m_sectorsPerTrack = m_image->sectorsPerTrack();
    m_cuePath = m_image->finish();
    QVERIFY2(!m_cuePath.isEmpty(), "Failed to write the test CD image");
}

void TestCdStream::testStartGeneratesWavStream()
{
    createTwoTrackImage();

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    stream.start();

    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 0, 0);
    QCOMPARE(stream.size(), 44 + qint64(m_sectorsPerTrack) * 2352);
    QCOMPARE(stream.position(), qint64(0));
    QVERIFY(stream.isSeekable());

    stream.stop();
}

void TestCdStream::testSequentialReadsAndEof()
{
    createTwoTrackImage();

    DragonCdStream stream;
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    stream.start();

    std::vector<uint8_t> data = readAllFromStream(stream, stream.size());
    QCOMPARE(qint64(data.size()), stream.size());

    // The WAV header must match a real header, and the PCM must match the
    // image bytes exactly.
    const auto header = DragonCdStream::makeWavHeader(uint32_t(m_sectorsPerTrack) * 2352);
    QVERIFY(std::equal(header.begin(), header.end(), data.begin()));
    const QByteArray pcm(reinterpret_cast<const char *>(data.data() + 44), qsizetype(data.size()) - 44);
    QCOMPARE(pcm, m_track1Pcm);

    // A read at EOF must return 0.
    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    QCOMPARE(stream.read(buffer, stopSource.get_token()), 0);

    stream.stop();
}

void TestCdStream::testSeek()
{
    createTwoTrackImage();

    DragonCdStream stream;
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    stream.start();

    const qint64 target = 44 + 37 * 2352 + 1234;
    QCOMPARE(stream.seek(target), target);
    QCOMPARE(stream.position(), target);

    std::vector<uint8_t> buffer(5000);
    std::stop_source stopSource;
    const int n = stream.read(buffer, stopSource.get_token());
    QCOMPARE(qint64(n), qint64(buffer.size()));

    const QByteArray expected(m_track1Pcm.mid(37 * 2352 + 1234, qsizetype(buffer.size())));
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(buffer.data()), qsizetype(n)), expected);

    // Seek back into the WAV header works too.
    QCOMPARE(stream.seek(10), 10);
    std::vector<uint8_t> small(34);
    const int m = stream.read(small, stopSource.get_token());
    QCOMPARE(m, 34);
    QCOMPARE(QByteArray(reinterpret_cast<const char *>(small.data()), 34),
             QByteArray(reinterpret_cast<const char *>(DragonCdStream::makeWavHeader(uint32_t(m_sectorsPerTrack) * 2352).data() + 10), 34));

    // Out-of-range seeks are clamped/rejected.
    QCOMPARE(stream.seek(-1), qint64(-1));
    QCOMPARE(stream.seek(stream.size() + 100000), stream.size());

    stream.stop();
}

void TestCdStream::testSmallAndLargeReadsAreConsistent()
{
    createTwoTrackImage();

    QByteArray small;
    {
        DragonCdStream stream;
        stream.setUrl(cdTrackUrl(u"Track 02.wav"_s, m_cuePath));
        stream.start();
        std::vector<uint8_t> buffer(13); // deliberately not sector-aligned
        std::stop_source stopSource;
        while (true) {
            const int n = stream.read(buffer, stopSource.get_token());
            if (n <= 0) {
                break;
            }
            small.append(reinterpret_cast<const char *>(buffer.data()), n);
        }
        stream.stop();
    }

    QByteArray large;
    {
        DragonCdStream stream;
        stream.setUrl(cdTrackUrl(u"Track 02.wav"_s, m_cuePath));
        stream.start();
        std::vector<uint8_t> buffer(100 * 1024);
        std::stop_source stopSource;
        while (true) {
            const int n = stream.read(buffer, stopSource.get_token());
            if (n <= 0) {
                break;
            }
            large.append(reinterpret_cast<const char *>(buffer.data()), n);
        }
        stream.stop();
    }

    QCOMPARE(small, large);
    QCOMPARE(small.size(), 44 + m_sectorsPerTrack * 2352);
}

void TestCdStream::testStopCancelsReads()
{
    createTwoTrackImage();

    DragonCdStream stream;
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    stream.start();

    std::vector<uint8_t> buffer(1024);
    std::stop_source stopSource;
    stopSource.request_stop();
    QCOMPARE(stream.read(buffer, stopSource.get_token()), -1);

    stream.stop();
    std::stop_source fresh;
    QCOMPARE(stream.read(buffer, fresh.get_token()), -1);

    DragonCdStream stoppedStream;
    stoppedStream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    stoppedStream.start();
    stoppedStream.stop();
    QCOMPARE(stoppedStream.read(buffer, fresh.get_token()), -1);
}

void TestCdStream::testErrorSlotCanStopStream()
{
    CdImage image;
    image.addAudioTrack();
    image.addDataTrack();
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    // A directly connected errorOccurred slot may reenter the stream API:
    // emission must happen with the I/O mutex released, or this deadlocks.
    DragonCdStream stream;
    QObject::connect(
        &stream,
        &DragonCdStream::errorOccurred,
        &stream,
        [&stream]() {
            stream.stop();
        },
        Qt::DirectConnection);
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 02.wav"_s, cue));
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 5000);
    QCOMPARE(stream.size(), qint64(-1));

    // The same contract holds for errors surfaced by read().
    createTwoTrackImage();

    DragonCdStream reading;
    QObject::connect(
        &reading,
        &DragonCdStream::errorOccurred,
        &reading,
        [&reading]() {
            reading.stop();
        },
        Qt::DirectConnection);
    QSignalSpy readErrorSpy(&reading, &DragonCdStream::errorOccurred);
    reading.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    reading.start();
    QVERIFY(reading.m_disc);

    reading.m_disc->invalidate();
    std::vector<uint8_t> buffer(64 * 1024);
    std::stop_source stopSource;
    QCOMPARE(reading.read(buffer, stopSource.get_token()), -1);
    QCOMPARE(readErrorSpy.count(), 1);
}

void TestCdStream::testDirectoryUrlFails()
{
    createTwoTrackImage();

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(QString(), m_cuePath));
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 5000);
    QCOMPARE(stream.size(), qint64(-1));
}

void TestCdStream::testMissingDeviceFails()
{
    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, u"/nonexistent/no-disc.cue"_s));
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 5000);
    QCOMPARE(stream.size(), qint64(-1));
}

void TestCdStream::testDataTrackFails()
{
    CdImage image;
    image.addAudioTrack();
    image.addDataTrack();
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 02.wav"_s, cue));
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 5000);
    QCOMPARE(stream.size(), qint64(-1));
}

void TestCdStream::testDataOnlyDiscFails()
{
    CdImage image;
    image.addDataTrack();
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, cue));
    stream.start();
    QTRY_VERIFY_WITH_TIMEOUT(errorSpy.count() > 0, 5000);
    QCOMPARE(stream.size(), qint64(-1));
}

void TestCdStream::testTrackQueryItemOverride()
{
    createTwoTrackImage();

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    // The file name says track 1, the query item says track 2.
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath, u"2"_s));
    stream.start();
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 0, 0);
    QCOMPARE(stream.size(), 44 + qint64(m_sectorsPerTrack) * 2352);

    // Track 3 does not exist on a two track image.
    DragonCdStream outOfRange;
    QSignalSpy outOfRangeSpy(&outOfRange, &DragonCdStream::errorOccurred);
    outOfRange.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath, u"3"_s));
    outOfRange.start();
    QTRY_VERIFY_WITH_TIMEOUT(outOfRangeSpy.count() > 0, 5000);

    stream.stop();
}

void TestCdStream::testFactoryRouting()
{
    createTwoTrackImage();

    // audiocd:/ URLs must route to DragonCdStream (priority over the KIO
    // fallback), classify as non-local, and be pre-warmable for gapless.
    const QUrl cdUrl = cdTrackUrl(u"Track 01.wav"_s, m_cuePath);
    QVERIFY(!DragonStreamFactory::isLocalSource(cdUrl));
    QVERIFY(DragonStreamFactory::supportsGaplessPreWarm(cdUrl));

    const auto cdStream = DragonStreamFactory::createStream(cdUrl);
    QVERIFY(cdStream != nullptr);
    QVERIFY(qobject_cast<DragonCdStream *>(cdStream.get()) != nullptr);

    // Scheme matching is case-insensitive in QUrl (schemes are lowercased).
    QUrl upperCaseScheme = cdUrl;
    upperCaseScheme.setScheme(u"AUDIOCD"_s);
    QVERIFY(qobject_cast<DragonCdStream *>(DragonStreamFactory::createStream(upperCaseScheme).get()) != nullptr);

    QCOMPARE(DragonStreamFactory::createStream(QUrl::fromLocalFile(m_cuePath)).get(), nullptr);
    const auto kioStream = DragonStreamFactory::createStream(QUrl(u"sftp://host/music/file.flac"_s));
    QVERIFY(qobject_cast<DragonKioStream *>(kioStream.get()) != nullptr);
}

void TestCdStream::testStreamsShareDisc()
{
    createTwoTrackImage();

    QTemporaryDir linkDir;
    const QString linkPath = linkDir.filePath(u"link.cue"_s);
    QVERIFY(QFile::link(m_cuePath, linkPath));

    std::weak_ptr<DragonCdDisc> weakDisc;
    {
        DragonCdStream current;
        current.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
        current.start();
        QVERIFY(current.m_disc);

        DragonCdStream next;
        next.setUrl(cdTrackUrl(u"Track 02.wav"_s, linkPath));
        next.start();
        QVERIFY(next.m_disc);
        QCOMPARE(next.m_disc.get(), current.m_disc.get());

        const auto data = readAllFromStream(current, current.size());
        const QByteArray pcm(reinterpret_cast<const char *>(data.data() + 44), qsizetype(data.size()) - 44);
        QCOMPARE(pcm, m_track1Pcm);
        QCOMPARE(qint64(readAllFromStream(next, next.size()).size()), next.size());

        weakDisc = current.m_disc;
        current.stop();
        QVERIFY(!weakDisc.expired());
        next.stop();
    }
    QVERIFY(weakDisc.expired());
}

void TestCdStream::testConcurrentAcquireOpensDiscOnce()
{
    createTwoTrackImage();

    QTemporaryDir linkDir;
    const QString linkPath = linkDir.filePath(u"link.cue"_s);
    QVERIFY(QFile::link(m_cuePath, linkPath));

    static int tocBuiltCount = 0;
    tocBuiltCount = 0;
    const QtMessageHandler previousHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        if (message.contains(u"DragonCdToc built"_s)) {
            ++tocBuiltCount;
        }
    });
    QLoggingCategory::setFilterRules(u"org.kde.dragonmediabackend.*.debug=true"_s);

    constexpr int kThreads = 8;
    std::barrier sync(kThreads);
    std::vector<std::thread> threads;
    std::vector<std::shared_ptr<DragonCdDisc>> discs{size_t(kThreads)};
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([this, i, &sync, &discs, &linkPath]() {
            sync.arrive_and_wait();
            auto disc = DragonCdDisc::acquire((i % 2) ? linkPath : m_cuePath);
            if (disc) {
                discs[size_t(i)] = *disc;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    QLoggingCategory::setFilterRules(QString{});
    qInstallMessageHandler(previousHandler);

    for (const auto &disc : discs) {
        QVERIFY(disc != nullptr);
    }
    for (size_t i = 1; i < discs.size(); ++i) {
        QCOMPARE(discs[i].get(), discs.front().get());
    }
    QCOMPARE(tocBuiltCount, 1);
}

void TestCdStream::testReadErrorInvalidatesDisc()
{
    createTwoTrackImage();

    DragonCdStream current;
    QSignalSpy errorSpy(&current, &DragonCdStream::errorOccurred);
    current.setUrl(cdTrackUrl(u"Track 01.wav"_s, m_cuePath));
    current.start();
    QVERIFY(current.m_disc);
    const auto stale = current.m_disc;

    stale->invalidate();
    std::vector<uint8_t> buffer(64 * 1024);
    std::stop_source stopSource;
    QCOMPARE(current.read(buffer, stopSource.get_token()), -1);
    QCOMPARE(errorSpy.count(), 1);

    DragonCdStream next;
    QSignalSpy nextErrorSpy(&next, &DragonCdStream::errorOccurred);
    next.setUrl(cdTrackUrl(u"Track 02.wav"_s, m_cuePath));
    next.start();
    QCOMPARE(nextErrorSpy.count(), 0);
    QVERIFY(next.m_disc);
    QVERIFY(next.m_disc.get() != stale.get());
    QVERIFY(next.m_disc->isValid());
    QCOMPARE(qint64(readAllFromStream(next, next.size()).size()), next.size());
}

void TestCdStream::testChangedImageReopensDisc()
{
    CdImage image;
    image.addAudioTrack();
    image.addAudioTrack();
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    DragonCdStream current;
    current.setUrl(cdTrackUrl(u"Track 01.wav"_s, cue));
    current.start();
    QVERIFY(current.m_disc);
    QCOMPARE(current.m_disc->lastTrack(), 2);

    {
        QFile cueFile(cue);
        QVERIFY(cueFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
        cueFile.write("FILE \"disc.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
    }

    DragonCdStream newDiscTrack1;
    QSignalSpy errorSpy(&newDiscTrack1, &DragonCdStream::errorOccurred);
    newDiscTrack1.setUrl(cdTrackUrl(u"Track 01.wav"_s, cue));
    newDiscTrack1.start();
    QCOMPARE(errorSpy.count(), 0);
    QVERIFY(newDiscTrack1.m_disc);
    QVERIFY(newDiscTrack1.m_disc.get() != current.m_disc.get());
    QCOMPARE(newDiscTrack1.m_disc->lastTrack(), 1);
    QCOMPARE(newDiscTrack1.size(), 44 + qint64(2 * image.sectorsPerTrack()) * 2352);

    DragonCdStream newDiscTrack2;
    QSignalSpy missingSpy(&newDiscTrack2, &DragonCdStream::errorOccurred);
    newDiscTrack2.setUrl(cdTrackUrl(u"Track 02.wav"_s, cue));
    newDiscTrack2.start();
    QCOMPARE(missingSpy.count(), 1);

    QCOMPARE(current.m_disc->lastTrack(), 2);
    QCOMPARE(qint64(readAllFromStream(current, current.size()).size()), current.size());
}

void TestCdStream::testGaplessPlayerTransition()
{
    createTwoTrackImage();

    DragonPlayer player;
    if (player.audioOutput()->backend() == DragonAudioOutput::Backend::Null) {
        QSKIP("No audio sink available");
    }
    PlayerHelper helper(&player);
    player.setPrefinishMark(1500ms);

    const QUrl track1 = cdTrackUrl(u"Track 01.wav"_s, m_cuePath);
    const QUrl track2 = cdTrackUrl(u"Track 02.wav"_s, m_cuePath);

    QVERIFY2(helper.setSourceAndWait(track1), "Track 1 failed to load");
    const auto duration1 = player.duration();
    QVERIFY(duration1.has_value());
    QVERIFY(*duration1 > 1500ms);
    QVERIFY(player.isSeekable());
    player.setNextSource(track2);

    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");

    QSignalSpy trackSpy(&player, &DragonPlayer::trackChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return trackSpy.count() > 0;
                 },
                 60000),
             "Gapless transition to track 2 did not happen");

    QVERIFY(player.source() == track2);
    QVERIFY(player.error() == DragonPlayer::Error::NoError);

    for (const auto &args : statusSpy) {
        const auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        QVERIFY2(status != DragonPlayer::MediaStatus::EndOfMedia, "EndOfMedia during gapless transition");
        QVERIFY2(status != DragonPlayer::MediaStatus::StalledMedia, "StalledMedia during gapless transition");
        QVERIFY2(status != DragonPlayer::MediaStatus::BufferingMedia, "BufferingMedia during gapless transition");
    }

    QTRY_COMPARE_WITH_TIMEOUT(player.duration().value_or(0ms), std::chrono::milliseconds(m_sectorsPerTrack * 1000 / 75), 5000);
    QVERIFY(player.isSeekable());

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return player.position() > 500ms;
                 },
                 30000),
             "Track 2 did not start producing audio");
}

void TestCdStream::testLateNextSourceGaplessTransition()
{
    createTwoTrackImage();

    DragonPlayer player;
    if (player.audioOutput()->backend() == DragonAudioOutput::Backend::Null) {
        QSKIP("No audio sink available");
    }
    PlayerHelper helper(&player);
    player.setPrefinishMark(100ms);

    const QUrl track1 = cdTrackUrl(u"Track 01.wav"_s, m_cuePath);
    const QUrl track2 = cdTrackUrl(u"Track 02.wav"_s, m_cuePath);

    QVERIFY2(helper.setSourceAndWait(track1), "Track 1 failed to load");

    bool queued = false;
    connect(&player, &DragonPlayer::aboutToFinish, &player, [&]() {
        if (!queued) {
            queued = true;
            player.setNextSource(track2);
        }
    });

    QSignalSpy trackSpy(&player, &DragonPlayer::trackChanged);
    QSignalSpy statusSpy(&player, &DragonPlayer::statusChanged);

    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return trackSpy.count() > 0;
                 },
                 60000),
             "Late gapless transition to track 2 did not happen");
    QVERIFY(queued);
    QVERIFY(player.source() == track2);
    QVERIFY(player.error() == DragonPlayer::Error::NoError);

    for (const auto &args : statusSpy) {
        const auto status = args.at(0).value<DragonPlayer::MediaStatus>();
        QVERIFY2(status != DragonPlayer::MediaStatus::EndOfMedia, "EndOfMedia during late gapless transition");
        QVERIFY2(status != DragonPlayer::MediaStatus::StalledMedia, "StalledMedia during late gapless transition");
        QVERIFY2(status != DragonPlayer::MediaStatus::LoadingMedia, "Track 2 was reloaded instead of handed off gaplessly");
    }

    QVERIFY(player.isSeekable());

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return player.position() > 500ms;
                 },
                 30000),
             "Track 2 did not start producing audio");
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
}

void TestCdStream::testGaplessCdTrackFollowedByLocalTrackPreservesSeekable()
{
    QVERIFY(TestFixture::verifyExists(u"sample-3s.mp3"_s));
    CdImage image(50);
    image.addAudioTrack();
    image.addAudioTrack();
    const QString cuePath = image.finish();
    QVERIFY(!cuePath.isEmpty());

    DragonPlayer player;
    if (player.audioOutput()->backend() == DragonAudioOutput::Backend::Null) {
        QSKIP("No audio sink available");
    }
    PlayerHelper helper(&player);
    player.setPrefinishMark(200ms);

    DragonDiagnostics diagnostics(&player);

    const QUrl track1 = cdTrackUrl(u"Track 01.wav"_s, cuePath);
    const QUrl track2 = cdTrackUrl(u"Track 02.wav"_s, cuePath);
    const QUrl track3 = QUrl::fromLocalFile(TestFixture::fixturePath(u"sample-3s.mp3"_s));

    QList<QUrl> sourcesSeen;
    QList<bool> seekableSeen;
    connect(&player, &DragonPlayer::trackChanged, &player, [&]() {
        sourcesSeen.append(player.source());
        seekableSeen.append(player.isSeekable());
        qDebug() << "trackChanged: source=" << player.source() << "seekable=" << player.isSeekable();
    });

    QVERIFY2(helper.setSourceAndWait(track1), "Track 1 failed to load");
    QVERIFY2(helper.playAndWait(), "Track 1 failed to start playing");

    QVERIFY2(QTest::qWaitFor(
                 [&diagnostics]() {
                     return !diagnostics.hasActiveDecoder();
                 },
                 10000),
             "Track 1 should decode to EOF while its tail is still playing");

    player.setNextSource(track2);
    QVERIFY2(QTest::qWaitFor(
                 [&diagnostics]() {
                     return diagnostics.hasActiveDecoder();
                 },
                 10000),
             "Track 2 decoder should be active after promotion");

    QVERIFY2(QTest::qWaitFor(
                 [&diagnostics]() {
                     return !diagnostics.hasActiveDecoder();
                 },
                 10000),
             "Track 2 should decode to EOF while track 1 is still playing");

    player.setNextSource(track3);
    QVERIFY2(QTest::qWaitFor(
                 [&diagnostics]() {
                     return diagnostics.hasActiveDecoder();
                 },
                 10000),
             "Track 3 decoder should be active after promotion");

    QVERIFY2(QTest::qWaitFor(
                 [&sourcesSeen, &track2]() {
                     return sourcesSeen.contains(track2);
                 },
                 10000),
             "The sink should reach track 2's boundary and emit trackChanged");

    const int track2Index = sourcesSeen.indexOf(track2);
    QVERIFY(track2Index >= 0);
    QVERIFY2(seekableSeen.at(track2Index), "Track 2 (CD track) must remain seekable after track 3 is promoted");
}

QTEST_MAIN(TestCdStream)
#include "test_cdstream.moc"
