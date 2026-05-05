/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <QImage>
#include <QWidget>
#include <mutex>
#include <vector>

class DragonSpectrogram : public QWidget
{
    Q_OBJECT

public:
    explicit DragonSpectrogram(QWidget *parent = nullptr);
    ~DragonSpectrogram() override;

    static constexpr int PreferredHeight = 200;

    static constexpr int HistorySize = 128;

public Q_SLOTS:

    void updateFrequencies(const std::vector<float> &frequenciesDb);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    int m_writeIndex = 0;
    bool m_historyDirty = false;
    std::mutex m_dataMutex;

    [[nodiscard]] static QRgb dbToColor(float db);
};
