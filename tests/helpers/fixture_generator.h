/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Generates deterministic FLAC fixtures with multi-sample boundary
 * signatures and low-amplitude background tone for gapless validation.
 * Uses libFLAC stream encoder to produce standard .flac files.
 */

#pragma once

#include <QString>
#include <QTemporaryFile>

#include <FLAC/stream_encoder.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct BoundaryFixture {
    QString filePath;
    int sampleRate;
    int channels;
    int totalFrames;
    std::vector<float> expectedSamples;
};

namespace FixtureGenerator
{

static constexpr std::array<float, 4> kEndSignature = {+1.0f, -1.0f, +1.0f, -1.0f};
static constexpr std::array<float, 4> kStartSignature = {-1.0f, +1.0f, -1.0f, +1.0f};
static constexpr float kToneAmplitude = 0.1f;
static constexpr double kEndToneFreq = 220.0;
static constexpr double kStartToneFreq = 330.0;
static constexpr int kBitsPerSample = 16;

inline std::vector<FLAC__int32> floatToInt32(const std::vector<float> &samples)
{
    constexpr float scale = static_cast<float>((1 << (kBitsPerSample - 1)) - 1);
    std::vector<FLAC__int32> out(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        out[i] = static_cast<FLAC__int32>(std::round(clamped * scale));
    }
    return out;
}

inline bool writeFlac(const QString &path, const std::vector<float> &samples, int sampleRate, int channels, int totalFrames)
{
    auto *encoder = FLAC__stream_encoder_new();
    if (!encoder) {
        return false;
    }

    FLAC__stream_encoder_set_verify(encoder, true);
    FLAC__stream_encoder_set_compression_level(encoder, 0);
    FLAC__stream_encoder_set_channels(encoder, static_cast<uint32_t>(channels));
    FLAC__stream_encoder_set_bits_per_sample(encoder, static_cast<uint32_t>(kBitsPerSample));
    FLAC__stream_encoder_set_sample_rate(encoder, static_cast<uint32_t>(sampleRate));
    FLAC__stream_encoder_set_total_samples_estimate(encoder, static_cast<FLAC__uint64>(totalFrames));

    QByteArray pathUtf8 = path.toUtf8();
    auto status = FLAC__stream_encoder_init_file(encoder, pathUtf8.constData(), nullptr, nullptr);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(encoder);
        return false;
    }

    auto intSamples = floatToInt32(samples);

    // FLAC expects channel-interleaved samples in process_interleaved
    if (!FLAC__stream_encoder_process_interleaved(encoder, intSamples.data(), static_cast<uint32_t>(totalFrames))) {
        FLAC__stream_encoder_delete(encoder);
        return false;
    }

    FLAC__stream_encoder_finish(encoder);
    FLAC__stream_encoder_delete(encoder);
    return true;
}

inline void generateSamples(int sampleRate,
                            int channels,
                            int durationFrames,
                            double toneFreq,
                            const std::array<float, 4> *signature,
                            bool signatureAtEnd,
                            std::vector<float> &out)
{
    const int totalSamples = durationFrames * channels;
    out.resize(totalSamples, 0.0f);

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = static_cast<float>(std::sin(2.0 * M_PI * toneFreq * t) * kToneAmplitude);
        out[frm * channels] = val;
        if (channels > 1) {
            out[frm * channels + 1] = val;
        }
    }

    if (signature) {
        int sigLen = static_cast<int>(signature->size());
        int sigStart = signatureAtEnd ? (durationFrames - sigLen) : 0;
        for (int k = 0; k < sigLen; ++k) {
            int frm = sigStart + k;
            float val = (*signature)[k];
            out[frm * channels] = val;
            if (channels > 1) {
                out[frm * channels + 1] = val;
            }
        }
    }
}

inline BoundaryFixture makeEndMarkerFixture(int sampleRate, int channels, int durationFrames)
{
    BoundaryFixture f;
    f.sampleRate = sampleRate;
    f.channels = channels;
    f.totalFrames = durationFrames;

    std::vector<float> samples;
    generateSamples(sampleRate, channels, durationFrames, kEndToneFreq, &kEndSignature, true, samples);

    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QStringLiteral("test-a-end-XXXXXX.flac"));
    (void)tmpFile.open();
    tmpFile.close();

    writeFlac(tmpFile.fileName(), samples, sampleRate, channels, durationFrames);

    f.filePath = tmpFile.fileName();
    f.expectedSamples = std::move(samples);
    tmpFile.setAutoRemove(false);
    return f;
}

inline BoundaryFixture makeStartMarkerFixture(int sampleRate, int channels, int durationFrames)
{
    BoundaryFixture f;
    f.sampleRate = sampleRate;
    f.channels = channels;
    f.totalFrames = durationFrames;

    std::vector<float> samples;
    generateSamples(sampleRate, channels, durationFrames, kStartToneFreq, &kStartSignature, false, samples);

    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QStringLiteral("test-b-start-XXXXXX.flac"));
    (void)tmpFile.open();
    tmpFile.close();

    writeFlac(tmpFile.fileName(), samples, sampleRate, channels, durationFrames);

    f.filePath = tmpFile.fileName();
    f.expectedSamples = std::move(samples);
    tmpFile.setAutoRemove(false);
    return f;
}

inline BoundaryFixture makeTenSecondFixture(int sampleRate, int channels, int durationFrames)
{
    BoundaryFixture f;
    f.sampleRate = sampleRate;
    f.channels = channels;
    f.totalFrames = durationFrames;

    const int totalSamples = durationFrames * channels;
    f.expectedSamples.resize(totalSamples, 0.0f);

    constexpr double kFreq = 440.0;
    constexpr float kAmplitude = 0.5f;

    for (int frm = 0; frm < durationFrames; ++frm) {
        double t = static_cast<double>(frm) / sampleRate;
        float val = static_cast<float>(std::sin(2.0 * M_PI * kFreq * t) * kAmplitude);
        f.expectedSamples[frm * channels] = val;
        if (channels > 1) {
            f.expectedSamples[frm * channels + 1] = val;
        }
    }

    // Overwrite first 4 frames with start marker
    const int sigLen = static_cast<int>(kStartSignature.size());
    for (int k = 0; k < sigLen; ++k) {
        float val = kStartSignature[k];
        f.expectedSamples[k * channels] = val;
        if (channels > 1) {
            f.expectedSamples[k * channels + 1] = val;
        }
    }

    // Overwrite last 4 frames with end marker
    const int endSigStart = durationFrames - sigLen;
    for (int k = 0; k < sigLen; ++k) {
        float val = kEndSignature[k];
        f.expectedSamples[(endSigStart + k) * channels] = val;
        if (channels > 1) {
            f.expectedSamples[(endSigStart + k) * channels + 1] = val;
        }
    }

    QTemporaryFile tmpFile;
    tmpFile.setFileTemplate(QStringLiteral("test-10sec-XXXXXX.flac"));
    (void)tmpFile.open();
    tmpFile.close();

    writeFlac(tmpFile.fileName(), f.expectedSamples, sampleRate, channels, durationFrames);

    f.filePath = tmpFile.fileName();
    tmpFile.setAutoRemove(false);
    return f;
}
}
