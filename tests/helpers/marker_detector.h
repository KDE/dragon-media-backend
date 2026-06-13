/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Sample-level marker search for gapless transition verification.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

struct MarkerHit {
    bool found = false;
    int64_t frameIndex = -1;
    float amplitude = 0.0f;
};

inline MarkerHit findEndMarker(const std::vector<float> &pcm, int channels)
{
    constexpr float threshold = 0.01f;
    MarkerHit hit;

    for (int64_t s = 0; s + channels <= static_cast<int64_t>(pcm.size()); s += channels) {
        float v = pcm[s];
        if (std::abs(v - 1.0f) < threshold) {
            hit.found = true;
            hit.frameIndex = s / channels;
            hit.amplitude = v;
            return hit;
        }
    }

    return hit;
}

inline MarkerHit findStartMarker(const std::vector<float> &pcm, int channels)
{
    constexpr float threshold = 0.01f;
    MarkerHit hit;

    for (int64_t s = 0; s + channels <= static_cast<int64_t>(pcm.size()); s += channels) {
        float v = pcm[s];
        if (std::abs(v + 1.0f) < threshold) {
            hit.found = true;
            hit.frameIndex = s / channels;
            hit.amplitude = v;
            return hit;
        }
    }

    return hit;
}

inline int64_t gapFrames(const MarkerHit &aEnd, const MarkerHit &bStart)
{
    if (!aEnd.found || !bStart.found) {
        return -1;
    }
    return bStart.frameIndex - aEnd.frameIndex - 1;
}