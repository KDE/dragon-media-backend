/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QWidget>
#include <vector>

class DragonVisualizer : public QWidget
{
    Q_OBJECT

public:
    explicit DragonVisualizer(QWidget *parent = nullptr);
    ~DragonVisualizer() override;

    static constexpr int PreferredHeight = 120;

public Q_SLOTS:

    void updateBarData(const std::vector<float> &barData);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    std::vector<float> m_barData;
    std::vector<float> m_peakData;
    std::vector<float> m_displayData;

    static constexpr float PeakDecayRate = 2.0f;
    static constexpr float SmoothingFactor = 0.4f;
    static constexpr float MinDb = -80.0f;
    static constexpr float MaxDb = 0.0f;

    void updateDisplayData();
    [[nodiscard]] float dbToNormalized(float db) const;
};
