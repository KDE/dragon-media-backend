/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonvisualizer.h"

#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>

DragonVisualizer::DragonVisualizer(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(80);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_barData.reserve(24);
    m_peakData.reserve(24);
    m_displayData.reserve(24);
}

DragonVisualizer::~DragonVisualizer() = default;

void DragonVisualizer::updateBarData(const std::vector<float> &barData)
{
    const size_t numBars = barData.size();

    if (m_barData.size() != numBars) {
        m_barData.resize(numBars);
        m_peakData.resize(numBars, MinDb);
        m_displayData.resize(numBars, MinDb);
    }

    m_barData = barData;

    for (size_t i = 0; i < numBars; ++i) {
        const float newValue = barData[i];

        if (newValue > m_peakData[i]) {
            m_peakData[i] = newValue;
        } else {
            m_peakData[i] -= PeakDecayRate;
            if (m_peakData[i] < newValue) {
                m_peakData[i] = newValue;
            }
            if (m_peakData[i] < MinDb) {
                m_peakData[i] = MinDb;
            }
        }
    }

    updateDisplayData();
    update();
}

void DragonVisualizer::updateDisplayData()
{
    const size_t numBars = m_barData.size();
    for (size_t i = 0; i < numBars; ++i) {
        const float targetValue = m_barData[i];
        m_displayData[i] += (targetValue - m_displayData[i]) * SmoothingFactor;
    }
}

float DragonVisualizer::dbToNormalized(float db) const
{
    float normalized = (db - MinDb) / (MaxDb - MinDb);
    return std::clamp(normalized, 0.0f, 1.0f);
}

void DragonVisualizer::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect rect = this->rect();
    const int width = rect.width();
    const int height = rect.height();

    painter.fillRect(rect, QColor(20, 20, 30));

    const size_t numBars = m_displayData.size();
    if (numBars == 0) {
        return;
    }

    constexpr int BarSpacing = 2;
    const int totalSpacing = static_cast<int>((numBars + 1) * BarSpacing);
    const int availableWidth = width - totalSpacing;
    const float barWidthF = static_cast<float>(availableWidth) / static_cast<float>(numBars);

    QLinearGradient gradient(0, height, 0, 0);
    gradient.setColorAt(0.0, QColor(0, 200, 80));
    gradient.setColorAt(0.5, QColor(255, 220, 0));
    gradient.setColorAt(0.85, QColor(255, 100, 0));
    gradient.setColorAt(1.0, QColor(255, 50, 50));

    constexpr int PeakIndicatorHeight = 2;

    for (size_t i = 0; i < numBars; ++i) {
        const float x = BarSpacing + static_cast<float>(i) * (barWidthF + static_cast<float>(BarSpacing));

        const float normalizedValue = dbToNormalized(m_displayData[i]);
        const int barHeight = static_cast<int>(normalizedValue * (height - 4));

        const float barX = x;
        const int barY = height - 2 - barHeight;

        if (barHeight > 0) {
            QRectF barRect(barX, static_cast<float>(barY), barWidthF, static_cast<float>(barHeight));

            QBrush barBrush(gradient);
            painter.fillRect(barRect, barBrush);

            painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
            painter.drawLine(QPointF(barX, barY + 2), QPointF(barX, height - 2));
        }

        const float normalizedPeak = dbToNormalized(m_peakData[i]);
        if (normalizedPeak > 0.01f) {
            const int peakY = height - 2 - static_cast<int>(normalizedPeak * (height - 4));
            painter.fillRect(QRectF(barX, static_cast<float>(peakY), barWidthF, static_cast<float>(PeakIndicatorHeight)), QColor(255, 255, 255, 180));
        }
    }

    painter.setPen(QPen(QColor(60, 60, 80), 1));
    painter.drawRect(rect.adjusted(0, 0, -1, -1));
}

void DragonVisualizer::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}
