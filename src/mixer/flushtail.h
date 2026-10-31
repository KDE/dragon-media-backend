/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"

#include <array>
#include <cstddef>
#include <span>

class PrimaryChannel;

/**
 * Audio-thread ~5 ms anti-click ramp-out snapshot captured on a live Flush.
 *
 * Separate from FadingChannel so isCrossfading() stays false during a
 * seek or hard-switch flush while draining safely across small callback quanta.
 * Runs exclusively on the audio callback thread (or while quiesced).
 */
class DRAGONMEDIABACKEND_EXPORT FlushTail
{
public:
    static constexpr std::size_t kMaxSamples = 960; // 5 ms @ 96 kHz stereo

    void capture(PrimaryChannel &primary, int sampleRate, int channels);

    [[nodiscard]] std::size_t mixInto(std::span<float> dst, std::size_t validSamples, int channels) noexcept;

    void clear() noexcept;

    [[nodiscard]] bool isActive() const noexcept
    {
        return m_offset < m_count;
    }

private:
    std::array<float, kMaxSamples> m_buffer{};
    std::size_t m_offset = 0;
    std::size_t m_count = 0;
};
