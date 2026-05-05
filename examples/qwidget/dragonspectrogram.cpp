/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonspectrogram.h"

#include <QPaintEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>

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

    int r = 0, g = 0, b = 0;

    if (t < 0.2f) {
        const float s = t / 0.2f;
        r = 0;
        g = 0;
        b = static_cast<int>(s * 128);
    } else if (t < 0.4f) {
        const float s = (t - 0.2f) / 0.2f;
        r = 0;
        g = static_cast<int>(s * 255);
        b = 128 + static_cast<int>(s * 127);
    } else if (t < 0.6f) {
        const float s = (t - 0.4f) / 0.2f;
        r = 0;
        g = 255;
        b = static_cast<int>((1.0f - s) * 255);
    } else if (t < 0.8f) {
        const float s = (t - 0.6f) / 0.2f;
        r = static_cast<int>(s * 255);
        g = 255;
        b = 0;
    } else {
        const float s = (t - 0.8f) / 0.2f;
        r = 255;
        g = static_cast<int>((1.0f - s) * 255);
        b = 0;
    }

    return qRgb(r, g, b);
}

void DragonSpectrogram::updateFrequencies(const std::vector<float> &frequenciesDb)
{
    std::lock_guard lock(m_dataMutex);

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
        std::lock_guard lock(m_dataMutex);
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
