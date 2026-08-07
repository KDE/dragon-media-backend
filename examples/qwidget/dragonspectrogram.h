/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <QImage>
#include <QWidget>
#include <mutex>
#include <span>

class DragonSpectrogram : public QWidget
{
    Q_OBJECT

public:
    explicit DragonSpectrogram(QWidget *parent = nullptr);
    ~DragonSpectrogram() override;

    static constexpr int PreferredHeight = 200;

    static constexpr int HistorySize = 128;

public Q_SLOTS:

    void updateFrequencies(std::span<const float> frequenciesDb);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage m_image;
    int m_writeIndex = 0;
    bool m_historyDirty = false;
    std::mutex m_dataMutex;

    [[nodiscard]] static QRgb dbToColor(float db);
};
