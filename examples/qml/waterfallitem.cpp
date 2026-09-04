/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "waterfallitem.h"
#include <QSGSimpleTextureNode>
#include <algorithm>

WaterfallItem::WaterfallItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

WaterfallItem::~WaterfallItem() = default;

void WaterfallItem::setVisualization(VisualizationController *visualization)
{
    if (m_visualization == visualization) {
        return;
    }
    m_visualization = visualization;
    updateConnection();
    Q_EMIT visualizationChanged();
}

void WaterfallItem::itemChange(ItemChange change, const ItemChangeData &data)
{
    if (change == ItemSceneChange) {
        updateConnection();
    }
    QQuickItem::itemChange(change, data);
}

void WaterfallItem::updateConnection()
{
    bool shouldConnect = m_visualization && window() != nullptr;

    if (shouldConnect && !m_pixelsConnection) {
        m_pixelsConnection = connect(m_visualization, &VisualizationController::pixelsReady, this, &WaterfallItem::onPixelsReady);
    } else if (!shouldConnect && m_pixelsConnection) {
        disconnect(m_pixelsConnection);
        m_pixelsConnection = {};
    }
}

VisualizationController *WaterfallItem::visualization() const
{
    return m_visualization;
}

void WaterfallItem::onPixelsReady(const QList<uint32_t> &pixels)
{
    if (!m_visualization) {
        return;
    }

    std::lock_guard lock(m_dataMutex);

    if (const size_t numBins = pixels.size(); m_image.width() != static_cast<int>(numBins) || m_image.height() != static_cast<int>(m_historySize)) {
        m_image = QImage(numBins, m_historySize, QImage::Format_ARGB32_Premultiplied);
        m_image.fill(0xff000000);
        m_writeIndex = 0;
        m_texture.clear();
    }

    const auto line = reinterpret_cast<uint32_t *>(m_image.scanLine(m_writeIndex));
    std::ranges::copy(pixels, line);

    m_writeIndex = (m_writeIndex + 1) % m_historySize;
    m_historyDirty = true;
    update();
}

QSGNode *WaterfallItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!window() || m_image.isNull())
        return oldNode;

    if (!oldNode) {
        oldNode = new QSGNode();
        const auto topNode = new QSGSimpleTextureNode();
        const auto bottomNode = new QSGSimpleTextureNode();
        topNode->setFiltering(QSGTexture::Linear);
        bottomNode->setFiltering(QSGTexture::Linear);
        oldNode->appendChildNode(topNode);
        oldNode->appendChildNode(bottomNode);
    }

    const auto topNode = static_cast<QSGSimpleTextureNode *>(oldNode->firstChild());
    const auto bottomNode = static_cast<QSGSimpleTextureNode *>(topNode->nextSibling());

    int currentWriteIndex = 0;
    int width = 0;
    int height = 0;

    std::unique_lock<std::mutex> lock(m_dataMutex);
    width = m_image.width();
    height = m_image.height();
    currentWriteIndex = m_writeIndex;

    if (m_historyDirty || !m_texture) {
        const QImage decoupledImage = m_image.copy();
        m_historyDirty = false;
        lock.unlock();

        QSGTexture *newTexture = window()->createTextureFromImage(decoupledImage);

        bottomNode->setTexture(newTexture);
        bottomNode->setOwnsTexture(false);

        topNode->setTexture(newTexture);
        topNode->setOwnsTexture(true);

        m_texture = newTexture;
    } else {
        lock.unlock();
    }

    const QRectF bounds = boundingRect();
    const float scaleY = bounds.height() / height;
    const float topSectionHeight = height - currentWriteIndex;

    topNode->setSourceRect(QRectF(0, currentWriteIndex, width, topSectionHeight));
    topNode->setRect(QRectF(0, 0, bounds.width(), topSectionHeight * scaleY));

    bottomNode->setSourceRect(QRectF(0, 0, width, currentWriteIndex));
    bottomNode->setRect(QRectF(0, topSectionHeight * scaleY, bounds.width(), currentWriteIndex * scaleY));

    return oldNode;
}
