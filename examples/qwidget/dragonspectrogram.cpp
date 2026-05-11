/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonspectrogram.h"

#include <QPaintEvent>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>

namespace
{
constexpr uint32_t viridisBase[] = {
    0xff440154, 0xff481567, 0xff482677, 0xff453781, 0xff404788, 0xff39568c, 0xff33638d, 0xff2d708e, 0xff287d8e, 0xff238a8d,
    0xff1f968b, 0xff20a387, 0xff29af7f, 0xff3cbd72, 0xff55c667, 0xff75d054, 0xff95d840, 0xffb8de29, 0xffdce319, 0xfffde725,
};

constexpr std::array<QRgb, 256> generateViridisLUT()
{
    std::array<QRgb, 256> lut{};
    constexpr size_t baseCount = std::size(viridisBase);

    for (size_t i = 0; i < 256; ++i) {
        const float t = static_cast<float>(i) / 255.0f;
        const float scaled = t * static_cast<float>(baseCount - 1);
        const int idx = static_cast<int>(scaled);
        const float factor = scaled - static_cast<float>(idx);

        const uint32_t c1 = viridisBase[std::clamp(idx, 0, static_cast<int>(baseCount) - 2)];
        const uint32_t c2 = viridisBase[std::clamp(idx + 1, 1, static_cast<int>(baseCount) - 1)];

        const auto channel = [](uint32_t c, int shift) {
            return static_cast<int>((c >> shift) & 0xFF);
        };
        const auto lerp = [factor](int v1, int v2) {
            return static_cast<int>(static_cast<float>(v1) + factor * static_cast<float>(v2 - v1));
        };

        const int r = lerp(channel(c1, 16), channel(c2, 16));
        const int g = lerp(channel(c1, 8), channel(c2, 8));
        const int b = lerp(channel(c1, 0), channel(c2, 0));

        lut[i] = qRgb(r, g, b);
    }
    return lut;
}

constexpr std::array<QRgb, 256> ViridisLUT = generateViridisLUT();
}

DragonSpectrogram::DragonSpectrogram(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

DragonSpectrogram::~DragonSpectrogram() = default;

QRgb DragonSpectrogram::dbToColor(float db)
{
    float t = (db + 80.0f) / 80.0f;
    t = std::clamp(t, 0.0f, 1.0f);

    const uint8_t idx = static_cast<uint8_t>(t * 255.0f);
    return ViridisLUT[idx];
}

void DragonSpectrogram::updateFrequencies(const std::vector<float> &frequenciesDb)
{
    std::scoped_lock lock(m_dataMutex);

    const int numBins = static_cast<int>(frequenciesDb.size());
    if (numBins == 0) {
        return;
    }

    if (m_image.width() != numBins || m_image.height() != HistorySize) {
        m_image = QImage(numBins, HistorySize, QImage::Format_RGB32);
        m_image.fill(Qt::black);
        m_writeIndex = 0;
    }

    QRgb *line = reinterpret_cast<QRgb *>(m_image.scanLine(m_writeIndex));
    for (int i = 0; i < numBins; ++i) {
        line[i] = dbToColor(frequenciesDb[i]);
    }

    m_writeIndex = (m_writeIndex + 1) % HistorySize;
    m_historyDirty = true;

    update();
}

void DragonSpectrogram::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRect bounds = rect();

    QImage imageToDraw;
    int currentWriteIndex = 0;

    {
        std::scoped_lock lock(m_dataMutex);
        if (m_image.isNull()) {
            painter.fillRect(bounds, Qt::black);
            return;
        }
        imageToDraw = m_image.copy();
        currentWriteIndex = m_writeIndex;
        m_historyDirty = false;
    }

    const int width = imageToDraw.width();
    const int height = imageToDraw.height();

    const float scaleY = static_cast<float>(bounds.height()) / height;

    const int topSectionHeight = height - currentWriteIndex;
    const float topSectionScreenHeight = topSectionHeight * scaleY;

    if (topSectionHeight > 0) {
        const QRectF source(0, currentWriteIndex, width, topSectionHeight);
        const QRectF target(0, 0, bounds.width(), topSectionScreenHeight);
        painter.drawImage(target, imageToDraw, source);
    }

    if (currentWriteIndex > 0) {
        const QRectF source(0, 0, width, currentWriteIndex);
        const QRectF target(0, topSectionScreenHeight, bounds.width(), currentWriteIndex * scaleY);
        painter.drawImage(target, imageToDraw, source);
    }
}
