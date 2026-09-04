/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */
#pragma once

#include "visualizationcontroller.h"
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QtQmlIntegration/qqmlintegration.h>
#include <mutex>

class QSGTexture;

class WaterfallItem : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(VisualizationController *visualization READ visualization WRITE setVisualization NOTIFY visualizationChanged)

    friend class WaterfallItemTest;
    friend class VisualizationStressTest;

public:
    explicit WaterfallItem(QQuickItem *parent = nullptr);
    ~WaterfallItem() override;

    VisualizationController *visualization() const;
    void setVisualization(VisualizationController *visualization);

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void itemChange(ItemChange change, const ItemChangeData &data) override;

Q_SIGNALS:
    void visualizationChanged();

private Q_SLOTS:
    void onPixelsReady(const QList<uint32_t> &pixels);

private:
    void updateConnection();
    QPointer<VisualizationController> m_visualization;
    QMetaObject::Connection m_pixelsConnection;

    std::mutex m_dataMutex;

    size_t m_historySize = 300;
    size_t m_writeIndex = 0;
    bool m_historyDirty = false;

    QImage m_image;
    QPointer<QSGTexture> m_texture;
};
