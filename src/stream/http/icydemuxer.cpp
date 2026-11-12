/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "icydemuxer.h"

#include <QRegularExpression>

using namespace Qt::StringLiterals;

void IcyDemuxer::reset(qint64 metaint)
{
    ++m_epoch;
    m_metaint = metaint;
    m_bytesRead = 0;
    m_pendingData.clear();
    m_lastMetadata.clear();
}

void IcyDemuxer::setIcyMetaInt(qint64 metaint)
{
    ++m_epoch;
    m_metaint = metaint;
    m_bytesRead = 0;
}

void IcyDemuxer::clearPendingBytes()
{
    ++m_epoch;
    m_metaint = 0;
    m_bytesRead = 0;
    m_pendingData.clear();
}

DragonIcyMetadata IcyDemuxer::parseBlock(QByteArrayView metadataBlock)
{
    // An ICY block is a list of `Key='Value';` assignments padded with NULs to
    // a multiple of sixteen bytes.
    if (const qsizetype nulIdx = metadataBlock.indexOf('\0'); nulIdx >= 0) {
        metadataBlock = metadataBlock.first(nulIdx);
    }

    DragonIcyMetadata icy;
    if (metadataBlock.isEmpty()) {
        return icy;
    }

    const QString text = metadataBlock.isValidUtf8() ? QString::fromUtf8(metadataBlock) : QString::fromLatin1(metadataBlock);

    static const QRegularExpression assignment(uR"~(([^\s=;]+)='(.*?)'(?=;|$))~"_s);

    QHash<QString, QString> customFields;

    QRegularExpressionMatchIterator matches = assignment.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch field = matches.next();
        QString value = field.captured(2).trimmed();
        value.replace(u"''"_s, u"'"_s);

        const QString key = field.captured(1);
        if (key == u"StreamTitle"_s) {
            icy.setStreamTitle(value);
        } else if (key == u"StreamUrl"_s) {
            icy.setStreamUrl(value);
        } else {
            customFields.insert(key, value);
        }
    }

    icy.setCustomFields(customFields);
    return icy;
}

std::optional<DragonIcyMetadata> IcyDemuxer::processBlock(QByteArrayView rawBlock)
{
    DragonIcyMetadata icy = parseBlock(rawBlock);
    if (icy == m_lastMetadata) {
        return std::nullopt;
    }
    m_lastMetadata = icy;
    return icy;
}
