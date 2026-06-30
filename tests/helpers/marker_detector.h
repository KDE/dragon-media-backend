/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Multi-sample signature search for gapless transition verification.
 */

#pragma once

#include "fixture_generator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct MarkerHit {
    bool found = false;
    int64_t frameIndex = -1;
    float amplitude = 0.0f;
};

inline MarkerHit findSignature(const std::vector<float> &pcm, int channels, const std::array<float, 4> &signature, float threshold)
{
    MarkerHit hit;
    const int sigLen = static_cast<int>(signature.size());
    const int64_t maxFrame = static_cast<int64_t>(pcm.size()) / channels;

    for (int64_t frm = 0; frm + sigLen <= maxFrame; ++frm) {
        bool match = true;
        for (int k = 0; k < sigLen; ++k) {
            if (std::abs(pcm[(frm + k) * channels] - signature[k]) > threshold) {
                match = false;
                break;
            }
        }
        if (match) {
            hit.found = true;
            hit.frameIndex = frm;
            hit.amplitude = pcm[frm * channels];
            return hit;
        }
    }
    return hit;
}

inline MarkerHit findEndMarker(const std::vector<float> &pcm, int channels)
{
    return findSignature(pcm, channels, FixtureGenerator::kEndSignature, 0.01f);
}

inline MarkerHit findStartMarker(const std::vector<float> &pcm, int channels)
{
    return findSignature(pcm, channels, FixtureGenerator::kStartSignature, 0.01f);
}

inline int64_t gapFrames(const MarkerHit &aEnd, const MarkerHit &bStart)
{
    if (!aEnd.found || !bStart.found) {
        return -1;
    }
    const int sigLen = static_cast<int>(FixtureGenerator::kEndSignature.size());
    return bStart.frameIndex - (aEnd.frameIndex + sigLen);
}