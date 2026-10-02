/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */
#pragma once

#include <DragonFftFrame>
#include <QObject>
#include <QtQmlIntegration/qqmlintegration.h>
#include <span>

class DragonSpectrumAnalyzer;

class VisualizationController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Managed by the example application object")
    Q_PROPERTY(QList<float> barData READ barData NOTIFY barDataChanged)
    Q_PROPERTY(bool computeFft READ computeFft WRITE setComputeFft NOTIFY computeFftChanged)

public:
    explicit VisualizationController(DragonSpectrumAnalyzer *analyzer, QObject *parent = nullptr);
    ~VisualizationController() override;

    [[nodiscard]] QList<float> barData() const;
    [[nodiscard]] bool computeFft() const;

public Q_SLOTS:
    void setComputeFft(bool computeFft);
    void onFftFrame(const DragonFftFrame &frame);

Q_SIGNALS:
    void barDataChanged();
    void computeFftChanged(bool computeFft);
    void frequenciesReady(const QList<float> &frequenciesDb);
    void pixelsReady(const QList<uint32_t> &pixels);

private:
    Q_DISABLE_COPY_MOVE(VisualizationController)

    void computeViridisPixels(std::span<const float> frequenciesDb);

    QList<float> m_barData;
    bool m_computeFft = false;
    DragonSpectrumAnalyzer *m_analyzer = nullptr;
};
