/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Tests for the CD TOC API: DragonCdTocReader, DragonCdToc and
 * DragonCdTrack, against generated bin/cue images read through libcdio's
 * bincue driver, so no CD hardware is required.
 */

#include <QtTest>

#include "helpers/cdimage.h"
#include "logging_timestamp_init.h"

#include "stream/cd/dragoncdstream.h"

#include "player/dragonduration.h"

#include <DragonCdToc>
#include <DragonCdTocReader>
#include <DragonCdTrack>

#include <QEventLoop>
#include <QFileInfo>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QThread>
#include <QThreadPool>
#include <QTimer>

#include <chrono>
#include <memory>
#include <stop_token>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;
using CdImageTestUtil::generateSectorPcm;

namespace
{

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

QByteArray trackPcm(int fromSector, int sectors)
{
    QByteArray pcm;
    for (int i = 0; i < sectors; ++i) {
        pcm.append(generateSectorPcm(fromSector + i));
    }
    return pcm;
}

}

class TestCdToc : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testOffsetsAndLengths();
    void testDataTracks();
    void testDurations();
    void testValueTypeContract();
    void testUrlRoundTrip();
    void testCddbTrackOffsetListShape();
    void testErrorPaths();
    void testAsyncContract();
    void testQmlValueType();
    void testQmlReader();

private:
    [[nodiscard]] DragonCdToc readToc(const QString &cuePath);
    [[nodiscard]] static std::unique_ptr<QObject> createObject(QQmlEngine &engine, const QByteArray &qml);

    std::unique_ptr<CdImageTestUtil::CdImage> m_image;
    QString m_cuePath;
    int m_sectorsPerTrack = 0;
};

DragonCdToc TestCdToc::readToc(const QString &cuePath)
{
    DragonCdTocReader reader;
    DragonCdToc result;
    QString failure;

    QEventLoop loop;
    QObject::connect(&reader, &DragonCdTocReader::tocReady, &loop, [&](const DragonCdToc &toc) {
        result = toc;
        loop.quit();
    });
    QObject::connect(&reader, &DragonCdTocReader::failed, &loop, [&](const QString &errorMessage) {
        failure = errorMessage;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);

    reader.requestToc(cuePath);
    loop.exec();

    if (!failure.isEmpty()) {
        qWarning() << "TOC read failed:" << failure;
    }
    return result;
}

void TestCdToc::testOffsetsAndLengths()
{
    m_image = std::make_unique<CdImageTestUtil::CdImage>();
    m_image->addAudioTrack();
    m_image->addAudioTrack();
    m_sectorsPerTrack = m_image->sectorsPerTrack();
    m_cuePath = m_image->finish();
    QVERIFY2(!m_cuePath.isEmpty(), "Failed to write the test CD image");

    const DragonCdToc toc = readToc(m_cuePath);

    QCOMPARE(toc.device(), QFileInfo(m_cuePath).canonicalFilePath());
    QCOMPARE(toc.tracks().size(), 2);
    QCOMPARE(toc.tracks().front().number(), 1);
    QCOMPARE(toc.tracks().back().number(), 2);

    const QList<DragonCdTrack> tracks = toc.tracks();
    for (int n = 0; n < 2; ++n) {
        const DragonCdTrack &track = tracks.at(n);
        QCOMPARE(track.number(), n + 1);
        QVERIFY(track.isAudio());
        QCOMPARE(track.startFrame(), 150 + n * m_sectorsPerTrack);
        QCOMPARE(track.lengthFrames(), m_sectorsPerTrack);
    }

    QCOMPARE(toc.leadOutFrame(), 150 + 2 * m_sectorsPerTrack);
}

void TestCdToc::testDataTracks()
{
    CdImageTestUtil::CdImage image;
    image.addAudioTrack();
    image.addDataTrack();
    const QString cue = image.finish();
    QVERIFY(!cue.isEmpty());

    const DragonCdToc toc = readToc(cue);

    QCOMPARE(toc.tracks().size(), 2);
    QVERIFY(toc.tracks().first().isAudio());
    QVERIFY(!toc.tracks().last().isAudio());

    // CDDB signatures include data tracks.
    const QList<quint32> offsets = toc.cddbTrackOffsetList();
    QCOMPARE(offsets, QList<quint32>({quint32(150), quint32(150 + image.sectorsPerTrack()), quint32(150 + 2 * image.sectorsPerTrack())}));
}

void TestCdToc::testDurations()
{
    const DragonCdToc toc = readToc(m_cuePath);

    for (const DragonCdTrack &track : toc.tracks()) {
        QCOMPARE(track.duration(), std::chrono::milliseconds(qint64(m_sectorsPerTrack) * 1000 / 75));
    }
    QCOMPARE(toc.discDuration(), std::chrono::milliseconds(qint64(2 * m_sectorsPerTrack) * 1000 / 75));

    // Neutral values on placeholders.
    QCOMPARE(DragonCdToc().discDuration(), std::chrono::milliseconds{});
}

void TestCdToc::testValueTypeContract()
{
    DragonCdToc copy;
    {
        const DragonCdToc toc = readToc(m_cuePath);
        copy = toc;
    }
    // The copy stays valid after the original is gone (implicitly shared).
    QCOMPARE(copy.tracks().size(), 2);
    QCOMPARE(copy.tracks().at(1).number(), 2);
    QCOMPARE(copy.leadOutFrame(), 150 + 2 * m_sectorsPerTrack);

    // Two reads of the same image describe the same disc.
    const DragonCdToc first = readToc(m_cuePath);
    const DragonCdToc second = readToc(m_cuePath);
    QVERIFY(first == second);
    QVERIFY(!(first != second));
    QCOMPARE(first, second);

    // Qt value-type move contract: a moved-from object stays usable and
    // becomes a placeholder.
    DragonCdToc moved = readToc(m_cuePath);
    const int movedLeadOut = moved.leadOutFrame();
    DragonCdToc moveTarget(std::move(moved));
    QCOMPARE(moveTarget.leadOutFrame(), movedLeadOut);
    QVERIFY(moved == DragonCdToc());
    QCOMPARE(moved.tracks().size(), 0);

    DragonCdToc moveAssigned;
    moveAssigned = std::move(moveTarget);
    QCOMPARE(moveAssigned.leadOutFrame(), movedLeadOut);
    QVERIFY(moveTarget == DragonCdToc());

    DragonCdTrack movedTrack = first.tracks().front();
    const int movedTrackNumber = movedTrack.number();
    DragonCdTrack trackMoveTarget(std::move(movedTrack));
    QCOMPARE(trackMoveTarget.number(), movedTrackNumber);
    QVERIFY(movedTrack == DragonCdTrack());

    // Default-constructed values are neutral placeholders.
    const DragonCdToc nullToc;
    QVERIFY(nullToc != first);
    QVERIFY(nullToc.tracks().isEmpty());
    QCOMPARE(nullToc.device(), QString());
    QCOMPARE(nullToc.leadOutFrame(), 0);

    // The engine-mandated default constructor produces a neutral null track.
    const DragonCdTrack nullTrack;
    QVERIFY(nullTrack != first.tracks().front());
    QCOMPARE(nullTrack.number(), 0);
    QCOMPARE(nullTrack.duration(), std::chrono::milliseconds{});
    QVERIFY(nullTrack.url().isEmpty());
}

void TestCdToc::testUrlRoundTrip()
{
    const DragonCdToc toc = readToc(m_cuePath);

    const DragonCdTrack track2 = toc.tracks().at(1);
    QVERIFY(!track2.url().isEmpty());
    QCOMPARE(track2.url().scheme(), u"audiocd"_s);
    QCOMPARE(track2.url().fileName(), u"Track 02.wav"_s);

    DragonCdStream stream;
    QSignalSpy errorSpy(&stream, &DragonCdStream::errorOccurred);
    stream.setUrl(track2.url());
    stream.start();
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 0, 0);

    std::vector<uint8_t> data = readAllFromStream(stream, stream.size());
    QCOMPARE(qint64(data.size()), stream.size());

    const QByteArray expectedPcm = trackPcm(m_sectorsPerTrack, m_sectorsPerTrack);
    const QByteArray pcm(reinterpret_cast<const char *>(data.data() + DragonCdStream::WAV_HEADER_SIZE),
                         qsizetype(data.size()) - DragonCdStream::WAV_HEADER_SIZE);
    QCOMPARE(pcm, expectedPcm);

    stream.stop();

    // A data track has no playable URL.
    CdImageTestUtil::CdImage mixed;
    mixed.addAudioTrack();
    mixed.addDataTrack();
    const QString cue = mixed.finish();
    QVERIFY(!cue.isEmpty());
    const DragonCdToc mixedToc = readToc(cue);
    QVERIFY(mixedToc.tracks().last().url().isEmpty());
}

void TestCdToc::testCddbTrackOffsetListShape()
{
    const DragonCdToc toc = readToc(m_cuePath);

    const QList<quint32> offsets = toc.cddbTrackOffsetList();
    QCOMPARE(offsets.size(), toc.tracks().size() + 1);
    QCOMPARE(offsets.last(), quint32(toc.leadOutFrame()));
    for (int i = 1; i < offsets.size(); ++i) {
        QVERIFY(offsets.at(i) > offsets.at(i - 1));
    }
    for (int i = 0; i < toc.tracks().size(); ++i) {
        QCOMPARE(offsets.at(i), quint32(toc.tracks().at(i).startFrame()));
    }

    QCOMPARE(DragonCdToc().cddbTrackOffsetList().size(), 0);
}

void TestCdToc::testErrorPaths()
{
    DragonCdTocReader reader;
    QSignalSpy failedSpy(&reader, &DragonCdTocReader::failed);
    QSignalSpy readySpy(&reader, &DragonCdTocReader::tocReady);
    reader.requestToc(u"/nonexistent/no-disc.cue"_s);
    QTRY_VERIFY_WITH_TIMEOUT(failedSpy.count() == 1, 10000);
    QCOMPARE(readySpy.count(), 0);
    QVERIFY(!failedSpy.at(0).at(0).toString().isEmpty());
}

void TestCdToc::testAsyncContract()
{
    DragonCdTocReader reader;

    // tocReady is delivered on the reader's (main) thread via a queued
    // invocation from the worker.
    QThread *deliveryThread = nullptr;
    QObject::connect(&reader, &DragonCdTocReader::tocReady, this, [&](const DragonCdToc &) {
        deliveryThread = QThread::currentThread();
    });
    reader.requestToc(m_cuePath);
    QTRY_VERIFY_WITH_TIMEOUT(deliveryThread != nullptr, 10000);
    QCOMPARE(deliveryThread, reader.thread());

    // A second request while one is in flight coalesces into it: exactly
    // one tocReady.
    QSignalSpy readySpy(&reader, &DragonCdTocReader::tocReady);
    reader.requestToc(m_cuePath);
    reader.requestToc(m_cuePath);
    QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() == 1, 10000);
    QTest::qWait(200);
    QCOMPARE(readySpy.count(), 1);
}

std::unique_ptr<QObject> TestCdToc::createObject(QQmlEngine &engine, const QByteArray &qml)
{
    QQmlComponent component(&engine);
    component.setData(qml, QUrl());
    if (component.status() != QQmlComponent::Ready) {
        qWarning() << component.errorString();
        return nullptr;
    }
    return std::unique_ptr<QObject>(component.create());
}

void TestCdToc::testQmlValueType()
{
    const DragonCdToc toc = readToc(m_cuePath);

    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("toc"), QVariant::fromValue(toc));

    const auto object = createObject(engine, QByteArrayLiteral(R"QML(
        import QtQml
        QtObject {
            property var trackCount: toc.tracks.length
            property var discDuration: toc.discDuration.formatted()
            property var trackNumber: toc.tracks[0].number
            property var trackDuration: toc.tracks[0].duration.seconds
            property var device: toc.device
        }
    )QML"));
    QVERIFY(object);

    QCOMPARE(object->property("trackCount").toInt(), 2);
    QCOMPARE(object->property("discDuration").toString(), DragonDuration(toc.discDuration()).formatted());
    QCOMPARE(object->property("trackNumber").toInt(), 1);
    QCOMPARE(object->property("trackDuration").toDouble(), double(m_sectorsPerTrack) / 75.0);
    QCOMPARE(object->property("device").toString(), toc.device());
}

void TestCdToc::testQmlReader()
{
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral(DRAGON_QML_IMPORT_PATH));

    const auto object = createObject(engine, QByteArrayLiteral(R"QML(
        import QtQml
        import org.kde.dragonmediabackend

        DragonCdTocReader {
            property string failure: ""
            property var tocTracks: -1
            onTocReady: (toc) => tocTracks = toc.tracks.length
            onFailed: (message) => failure = message
            function poke() { requestToc("/nonexistent/no-disc.cue") }
        }
    )QML"));
    QVERIFY(object);

    QVERIFY(QMetaObject::invokeMethod(object.get(), "poke"));
    QTRY_VERIFY_WITH_TIMEOUT(!object->property("failure").toString().isEmpty(), 10000);
    QCOMPARE(object->property("tocTracks").toInt(), -1);
}

QTEST_GUILESS_MAIN(TestCdToc)
#include "test_cd_toc.moc"
