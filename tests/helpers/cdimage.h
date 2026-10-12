/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Generates deterministic bin/cue CD-DA images for tests: audio sectors
 * carry a distinct per-sector stereo sine signature, so offset errors are
 * detectable, and mixed-mode images can append a data track. The images
 * are read through libcdio's bincue driver, so no CD hardware is needed.
 */

#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cmath>

using namespace Qt::StringLiterals;

namespace CdImageTestUtil
{

constexpr qsizetype kSectorSize = 2352;
constexpr int kSamplesPerSector = 1176; // 16-bit stereo

inline QByteArray generateSectorPcm(int sectorIndex)
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

class CdImage
{
public:
    explicit CdImage(int sectorsPerTrack = 300)
        : m_sectorsPerTrack(sectorsPerTrack)
    {
    }

    // Appends an audio track and returns its PCM (little-endian).
    QByteArray addAudioTrack()
    {
        QByteArray trackPcm;
        for (int i = 0; i < m_sectorsPerTrack; ++i) {
            const QByteArray pcm = generateSectorPcm(m_sectorsWritten + i);
            trackPcm.append(pcm);
            m_bin.append(pcm);
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

} // namespace CdImageTestUtil
