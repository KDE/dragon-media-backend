/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "visualizationcontroller.h"
#include <DragonSpectrumAnalyzer>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <ranges>

namespace
{
constexpr std::array<uint32_t, 256> generateViridisLUT()
{
    std::array<uint32_t, 256> lut{};

    constexpr uint32_t viridis[] = {0xff440154, 0xff481567, 0xff482677, 0xff453781, 0xff404788, 0xff39568c, 0xff33638d, 0xff2d708e, 0xff287d8e, 0xff238a8d,
                                    0xff1f968b, 0xff20a387, 0xff29af7f, 0xff3cbd72, 0xff55c667, 0xff75d054, 0xff95d840, 0xffb8de29, 0xffdce319, 0xfffde725};

    for (int i = 0; i < 256; ++i) {
        const float t = i / 255.0f;
        const float scaled = t * (std::size(viridis) - 1);
        const int idx = std::clamp(static_cast<int>(scaled), 0, static_cast<int>(std::size(viridis) - 2));
        const float factor = scaled - idx;

        const uint32_t c1 = viridis[idx];
        const uint32_t c2 = viridis[idx + 1];

        const auto get_r = [](const uint32_t c) {
            return (c >> 16) & 0xFF;
        };
        const auto get_g = [](const uint32_t c) {
            return (c >> 8) & 0xFF;
        };
        const auto get_b = [](const uint32_t c) {
            return c & 0xFF;
        };

        const auto lerpChannel = [factor](const int v1, const int v2) {
            return static_cast<int>(std::lerp(static_cast<float>(v1), static_cast<float>(v2), factor));
        };

        const int r = lerpChannel(get_r(c1), get_r(c2));
        const int g = lerpChannel(get_g(c1), get_g(c2));
        const int b = lerpChannel(get_b(c1), get_b(c2));

        lut[i] = (255u << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    return lut;
}

constexpr std::array<uint32_t, 256> ViridisLUT = generateViridisLUT();
}

VisualizationController::VisualizationController(DragonSpectrumAnalyzer *analyzer, QObject *parent)
    : QObject(parent)
    , m_analyzer(analyzer)
{
    if (m_analyzer) {
        connect(m_analyzer, &DragonSpectrumAnalyzer::frameReady, this, &VisualizationController::onFftFrame);
    }
}

VisualizationController::~VisualizationController() = default;

QList<float> VisualizationController::barData() const
{
    return m_barData;
}

bool VisualizationController::computeFft() const
{
    return m_computeFft;
}

void VisualizationController::setComputeFft(bool computeFft)
{
    if (m_computeFft != computeFft) {
        m_computeFft = computeFft;
        Q_EMIT computeFftChanged(m_computeFft);
        if (m_analyzer) {
            m_analyzer->setMode(m_computeFft ? DragonSpectrumAnalyzer::Mode::Both : DragonSpectrumAnalyzer::Mode::Off);
        }
    }
}

void VisualizationController::onFftFrame(const DragonFftFrame &frame)
{
    QList<float> newBarData(frame.bars().begin(), frame.bars().end());
    if (m_barData != newBarData) {
        m_barData = newBarData;
        Q_EMIT barDataChanged();
    }

    QList<float> freqsDb(frame.frequencies().begin(), frame.frequencies().end());
    Q_EMIT frequenciesReady(freqsDb);

    computeViridisPixels(frame.frequencies());
}

void VisualizationController::computeViridisPixels(std::span<const float> frequenciesDb)
{
    auto toPixel = [](const float logBin) {
        const float val = (logBin + 80.0f) / 80.0f;
        const uint8_t idx = static_cast<uint8_t>(std::clamp(val, 0.0f, 1.0f) * 255.0f);
        return ViridisLUT[idx];
    };

    QList<uint32_t> pixels;
    pixels.reserve(static_cast<int>(frequenciesDb.size()));
    for (float db : frequenciesDb) {
        pixels.append(toPixel(db));
    }
    Q_EMIT pixelsReady(pixels);
}

#include "moc_visualizationcontroller.cpp"
