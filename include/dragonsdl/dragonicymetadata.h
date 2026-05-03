/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"

#include <QHash>
#include <QSharedDataPointer>
#include <QString>

class DragonIcyMetadataPrivate;

class DRAGONSDL_EXPORT DragonIcyMetadata
{
public:
    DragonIcyMetadata();
    DragonIcyMetadata(const DragonIcyMetadata &other);
    DragonIcyMetadata &operator=(const DragonIcyMetadata &other);
    ~DragonIcyMetadata();

    [[nodiscard]] QString streamTitle() const;

    [[nodiscard]] bool hasStreamTitle() const;

    void setStreamTitle(const QString &title);

    [[nodiscard]] QString streamUrl() const;

    [[nodiscard]] bool hasStreamUrl() const;

    void setStreamUrl(const QString &url);

    [[nodiscard]] QHash<QString, QString> customFields() const;

    void setCustomFields(const QHash<QString, QString> &fields);
    void insertCustomField(const QString &key, const QString &value);

    [[nodiscard]] bool isNull() const;

    void clear();

    [[nodiscard]] bool operator==(const DragonIcyMetadata &other) const;
    [[nodiscard]] bool operator!=(const DragonIcyMetadata &other) const;

private:
    QSharedDataPointer<DragonIcyMetadataPrivate> d;
};
