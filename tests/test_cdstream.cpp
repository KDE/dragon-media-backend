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

#include "logging_timestamp_init.h"
#include "test_utils.h"

#include "stream/dragoncddisc.h"
#include "stream/dragoncdstream.h"
#include "stream/dragonkiostream.h"
#include "stream/dragonstreamfactory.h"

#include <DragonAudioOutput>
#include <DragonPlayer>

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>

#include <cmath>
#include <stop_token>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace
{

constexpr qsizetype kSectorSize = 2352;
constexpr int kSamplesPerSector = 1176; // 16-bit stereo

QByteArray generateSectorPcm(int sectorIndex)
{
    // Smooth stereo sine wave, distinct per sector so offset errors are
    // detectable: left channel 440 Hz, right channel 660 Hz.
    QByteArray pcm(kSectorSize, Qt::Uninitialized);
    auto *samples = reinterpret_cast<int16_t *>(pcm.data());
    const double timeOffset = double(sectorIndex) * kSamplesPerSector / 2.0 / 44100.0;
    for (int i = 0; i < kSamplesPerSector; i += 2) {
        const double t = timeOffset + double(i / 2) / 44100.0;
        samples[i] = int16_t(11000.0 * std::sin(2.0 * M_PI * 440.0 * t));
        samples[i + 1] = int16_t(9000.0 * std::sin(2.0 * M_PI * 660.0 * t));
    }
    return pcm;
}

QByteArray byteSwap16(const QByteArray &data)
{
    QByteArray swapped(data);
    auto *samples = reinterpret_cast<uint16_t *>(swapped.data());
    const qsizetype count = swapped.size() / 2;
    for (qsizetype i = 0; i < count; ++i) {
        const uint16_t v = samples[i];
        samples[i] = uint16_t((v >> 8) | (v << 8));
    }
    return swapped;
}

class CdImage
{
public:
    explicit CdImage(int sectorsPerTrack = 300)
        : m_sectorsPerTrack(sectorsPerTrack)
    {
    }

    // Appends an audio track and returns its PCM (little-endian).
    QByteArray addAudioTrack(bool swapEndian = false)
    {
        QByteArray trackPcm;
        for (int i = 0; i < m_sectorsPerTrack; ++i) {
            const int sectorIndex = m_sectorsWritten + i;
            const QByteArray pcm = generateSectorPcm(sectorIndex);
            trackPcm.append(swapEndian ? byteSwap16(pcm) : pcm);
            m_bin.append(swapEndian ? byteSwap16(pcm) : pcm);
        }
        ++m_trackCount;
        m_sectorsWritten += m_sectorsPerTrack;
        m_trackTypes.append(u"AUDIO"_s);
        return trackPcm;
    }

    // Appends a data track (raw 2352-byte sectors declared as MODE1/2352).
    void addDataTrack()
    {
        m_bin.append(QByteArray(kSectorSize * m_sectorsPerTrack, 0));
        ++m_trackCount;
        m_sectorsWritten += m_sectorsPerTrack;
        m_trackTypes.append(u"MODE1/2352"_s);
    }

    QString finish()
    {
        const QString dir = m_tempDir.path();
        m_binPath = dir + u"/disc.bin";
        m_cuePath = dir + u"/disc.cue";

        QFile binFile(m_binPath);
        if (!binFile.open(QIODevice::WriteOnly)) {
            return {};
        }
        binFile.write(m_bin);
        binFile.close();

        QString cue = u"FILE \"disc.bin\" BINARY\n"_s;
        const int framesPerTrack = m_sectorsPerTrack; // 75 sectors per second
        for (int t = 0; t < m_trackCount; ++t) {
            cue += u"  TRACK %1 %2\n"_s.arg(t + 1, 2, 10, QChar(u'0')).arg(m_trackTypes.at(t));
            const int frame = t * framesPerTrack;
            cue += u"    INDEX 01 %1:%2:%3\n"_s.arg(frame / (75 * 60), 2, 10, QChar(u'0'))
                       .arg((frame / 75) % 60, 2, 10, QChar(u'0'))
                       .arg(frame % 75, 2, 10, QChar(u'0'));
        }

        QFile cueFile(m_cuePath);
        if (!cueFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return {};
        }
        cueFile.write(cue.toUtf8());
        cueFile.close();

        return m_cuePath;
    }

    [[nodiscard]] QString cuePath() const
    {
        return m_cuePath;
    }

    [[nodiscard]] int sectorsPerTrack() const
    {
        return m_sectorsPerTrack;
    }

private:
    QTemporaryDir m_tempDir;
    const int m_sectorsPerTrack;
    int m_trackCount = 0;
    int m_sectorsWritten = 0;
    QByteArray m_bin;
    QStringList m_trackTypes;
    QString m_binPath;
    QString m_cuePath;
};

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
    void testDirectoryUrlFails();
    void testMissingDeviceFails();
    void testDataTrackFails();
    void testDataOnlyDiscFails();
    void testTrackQueryItemOverride();
    void testEndiannessSwap();
    void testFactoryRouting();
    void testStreamsShareDisc();
    void testReadErrorInvalidatesDisc();
    void testChangedImageReopensDisc();
    void testGaplessPlayerTransition();
    void testLateNextSourceGaplessTransition();

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

    QCOMPARE(DragonCdStream::trackNumberFromName(u"Track 01.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"Track 1.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"Track 1.cda"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"Artist - 03 - Title.flac"_s, first, last).value_or(-1), 3);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"U2 - 01 - Beautiful Day.wav"_s, first, last).value_or(-1), 1);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"16_-_Some_Title.ogg"_s, first, last).value_or(-1), -1); // out of range
    QCOMPARE(DragonCdStream::trackNumberFromName(u"NoDigitsHere.wav"_s, first, last).value_or(-1), -1);
    QCOMPARE(DragonCdStream::trackNumberFromName(u"Track 99.wav"_s, first, last).value_or(-1), -1);
    // First valid run wins when there is no zero-padded two-digit run.
    QCOMPARE(DragonCdStream::trackNumberFromName(u"Track 3.wav"_s, first, last).value_or(-1), 3);
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

void TestCdStream::testEndiannessSwap()
{
    // A byte-swapped (big-endian) track must be transparently converted back
    // to little-endian by the drive endianness heuristic.
    CdImage image;
    const QByteArray swappedPcm = image.addAudioTrack(true);
    image.addAudioTrack(false);
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(cdTrackUrl(u"Track 01.wav"_s, cue));
    stream.start();
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 0, 0);

    const auto data = readAllFromStream(stream, stream.size());
    QCOMPARE(qint64(data.size()), stream.size());
    const QByteArray pcm(reinterpret_cast<const char *>(data.data() + 44), qsizetype(data.size()) - 44);
    QVERIFY(pcm != swappedPcm); // the image data is byte-swapped...
    QCOMPARE(pcm, byteSwap16(swappedPcm)); // ...and comes out as proper LE PCM

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
        QVERIFY(current.m_disc->swapLeBe().has_value());

        DragonCdStream next;
        next.setUrl(cdTrackUrl(u"Track 02.wav"_s, linkPath));
        next.start();
        QVERIFY(next.m_disc);
        QCOMPARE(next.m_disc.get(), current.m_disc.get());
        QCOMPARE(next.m_swapLeBe, *current.m_disc->swapLeBe());

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
    QCOMPARE(current.read(buffer, stopSource.get_token()), int(DragonCdStream::WAV_HEADER_SIZE));
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(current.read(buffer, stopSource.get_token()), -1);

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
    QVERIFY(player.seekable());
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
    QVERIFY(player.seekable());

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

    QVERIFY(player.seekable());

    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                     return player.position() > 500ms;
                 },
                 30000),
             "Track 2 did not start producing audio");
    QVERIFY(player.playbackState() == DragonPlayer::PlaybackState::PlayingState);
}

QTEST_MAIN(TestCdStream)
#include "test_cdstream.moc"
