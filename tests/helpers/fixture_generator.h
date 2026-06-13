/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Generates deterministic F32 WAV fixtures with multi-sample boundary
 * signatures and low-amplitude background tone for gapless validation.
 */

#pragma once

#include <QString>
#include <QTemporaryFile>

#include <array>
#include <cmath>
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

static constexpr std::array<float, 4> kEndSignature = {+1.0f, -1.0f, +1.0f, -1.0f};
static constexpr std::array<float, 4> kStartSignature = {-1.0f, +1.0f, -1.0f, +1.0f};
static constexpr float kToneAmplitude = 0.1f;
static constexpr double kEndToneFreq = 220.0;
static constexpr double kStartToneFreq = 330.0;

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

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = static_cast<float>(std::sin(2.0 * M_PI * kEndToneFreq * t) * kToneAmplitude);
        f.expectedSamples[frm * channels] = val;
        if (channels > 1) {
            f.expectedSamples[frm * channels + 1] = val;
        }
    }

    const int sigStart = durationFrames - static_cast<int>(kEndSignature.size());
    for (int k = 0; k < static_cast<int>(kEndSignature.size()); ++k) {
        int frm = sigStart + k;
        float val = kEndSignature[k];
        f.expectedSamples[frm * channels] = val;
        if (channels > 1) {
            f.expectedSamples[frm * channels + 1] = val;
        }
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

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = static_cast<float>(std::sin(2.0 * M_PI * kStartToneFreq * t) * kToneAmplitude);
        f.expectedSamples[frm * channels] = val;
        if (channels > 1) {
            f.expectedSamples[frm * channels + 1] = val;
        }
    }

    for (int k = 0; k < static_cast<int>(kStartSignature.size()); ++k) {
        int frm = k;
        float val = kStartSignature[k];
        f.expectedSamples[frm * channels] = val;
        if (channels > 1) {
            f.expectedSamples[frm * channels + 1] = val;
        }
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