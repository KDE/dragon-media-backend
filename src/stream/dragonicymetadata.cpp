/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <DragonMultimedia/dragonicymetadata.h>

class DragonIcyMetadataPrivate : public QSharedData
{
public:
    QString streamTitle;
    QString streamUrl;
    QHash<QString, QString> customFields;
};

DragonIcyMetadata::DragonIcyMetadata()
    : d(new DragonIcyMetadataPrivate)
{
}

DragonIcyMetadata::DragonIcyMetadata(const DragonIcyMetadata &other)
    : d(other.d)
{
}

DragonIcyMetadata &DragonIcyMetadata::operator=(const DragonIcyMetadata &other)
{
    d = other.d;
    return *this;
}

DragonIcyMetadata::~DragonIcyMetadata() = default;

QString DragonIcyMetadata::streamTitle() const
{
    return d->streamTitle;
}

bool DragonIcyMetadata::hasStreamTitle() const
{
    return !d->streamTitle.isEmpty();
}

void DragonIcyMetadata::setStreamTitle(const QString &title)
{
    d->streamTitle = title;
}

QString DragonIcyMetadata::streamUrl() const
{
    return d->streamUrl;
}

bool DragonIcyMetadata::hasStreamUrl() const
{
    return !d->streamUrl.isEmpty();
}

void DragonIcyMetadata::setStreamUrl(const QString &url)
{
    d->streamUrl = url;
}

QHash<QString, QString> DragonIcyMetadata::customFields() const
{
    return d->customFields;
}

void DragonIcyMetadata::setCustomFields(const QHash<QString, QString> &fields)
{
    d->customFields = fields;
}

void DragonIcyMetadata::insertCustomField(const QString &key, const QString &value)
{
    d->customFields.insert(key, value);
}

bool DragonIcyMetadata::isNull() const
{
    return d->streamTitle.isEmpty() && d->streamUrl.isEmpty() && d->customFields.isEmpty();
}

void DragonIcyMetadata::clear()
{
    d.reset(new DragonIcyMetadataPrivate);
}

bool DragonIcyMetadata::operator==(const DragonIcyMetadata &other) const
{
    return d->streamTitle == other.d->streamTitle && d->streamUrl == other.d->streamUrl && d->customFields == other.d->customFields;
}

bool DragonIcyMetadata::operator!=(const DragonIcyMetadata &other) const
{
    return !(*this == other);
}
