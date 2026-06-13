/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Generates deterministic F32 stereo WAV fixtures with boundary markers
 * for gapless playback validation.
 */

#pragma once

#include <QString>
#include <QTemporaryFile>

#include <cstdint>
#include <vector>

struct BoundaryFixture {
    QString filePath;
    std::vector<float> expectedSamples;
    int sampleRate;
    int channels;
    int totalFrames;
};

namespace FixtureGenerator
{

inline void writeWavHeader(QFile &file, int sampleRate, int channels, int totalFrames)
{
    const int byteRate = sampleRate * channels * sizeof(float);
    const int dataSize = totalFrames * channels * sizeof(float);

    auto write = [&](auto val) {
        (void)file.write(reinterpret_cast<const char *>(&val), sizeof(val));
    };
    auto write4cc = [&](const char s[4]) {
        (void)file.write(s, 4);
    };

    write4cc("RIFF");
    write(static_cast<uint32_t>(36 + dataSize));
    write4cc("WAVE");
    write4cc("fmt ");
    write(static_cast<uint32_t>(16));
    write(static_cast<uint16_t>(3));
    write(static_cast<uint16_t>(channels));
    write(static_cast<uint32_t>(sampleRate));
    write(static_cast<uint32_t>(byteRate));
    write(static_cast<uint16_t>(channels * sizeof(float)));
    write(static_cast<uint16_t>(sizeof(float) * 8));
    write4cc("data");
    write(static_cast<uint32_t>(dataSize));
}

BoundaryFixture makeEndMarkerFixture(int sampleRate, int channels, int durationFrames)
{
    BoundaryFixture f;
    f.sampleRate = sampleRate;
    f.channels = channels;
    f.totalFrames = durationFrames;

    const int totalSamples = durationFrames * channels;
    f.expectedSamples.resize(totalSamples, 0.0f);

    const double freq = 1000.0;
    const int rampFrames = 50;
    const int rampStart = durationFrames - rampFrames;

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = 0.0f;

        if (frm >= rampStart) {
            double rampFrac = static_cast<double>(frm - rampStart) / (rampFrames - 1);
            val = static_cast<float>(std::sin(2.0 * M_PI * freq * t) * rampFrac);
        }

        if (frm == durationFrames - 1) {
            val = 1.0f;
        }

        f.expectedSamples[frm * channels] = val;
    }

    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QStringLiteral("test-a-end-XXXXXX.wav"));
    (void)tmpFile.open();
    writeWavHeader(tmpFile, sampleRate, channels, durationFrames);

    for (int frm = 0; frm < durationFrames; ++frm) {
        for (int ch = 0; ch < channels; ++ch) {
            float v = f.expectedSamples[frm * channels + ch];
            tmpFile.write(reinterpret_cast<const char *>(&v), sizeof(v));
        }
    }

    tmpFile.flush();
    f.filePath = tmpFile.fileName();
    tmpFile.setAutoRemove(false);
    return f;
}

BoundaryFixture makeStartMarkerFixture(int sampleRate, int channels, int durationFrames)
{
    BoundaryFixture f;
    f.sampleRate = sampleRate;
    f.channels = channels;
    f.totalFrames = durationFrames;

    const int totalSamples = durationFrames * channels;
    f.expectedSamples.resize(totalSamples, 0.0f);

    const double freq = 880.0;
    const int rampFrames = 50;

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = 0.0f;

        if (frm < rampFrames) {
            double rampFrac = 1.0 - static_cast<double>(frm) / (rampFrames - 1);
            val = static_cast<float>(std::sin(2.0 * M_PI * freq * t) * rampFrac);
        }

        if (frm == 0) {
            val = -1.0f;
        }

        f.expectedSamples[frm * channels] = val;
    }

    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QStringLiteral("test-b-start-XXXXXX.wav"));
    (void)tmpFile.open();
    writeWavHeader(tmpFile, sampleRate, channels, durationFrames);

    for (int frm = 0; frm < durationFrames; ++frm) {
        for (int ch = 0; ch < channels; ++ch) {
            float v = f.expectedSamples[frm * channels + ch];
            tmpFile.write(reinterpret_cast<const char *>(&v), sizeof(v));
        }
    }

    tmpFile.flush();
    f.filePath = tmpFile.fileName();
    tmpFile.setAutoRemove(false);
    return f;
}

}