/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <QtGlobal>

using namespace Qt::StringLiterals;

namespace PaSinkInputProps
{

inline bool sinkInputPropsByApplicationName(const QString &applicationName, QJsonObject &propsOut)
{
    QProcess pactl;
    pactl.start(u"pactl"_s, QStringList{u"--format"_s, u"json"_s, u"list"_s, u"sink-inputs"_s});
    if (!pactl.waitForStarted(2000) || !pactl.waitForFinished(5000)) {
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(pactl.readAllStandardOutput());
    if (!doc.isArray()) {
        return false;
    }

    for (const QJsonValue &entry : doc.array()) {
        if (!entry.isObject()) {
            continue;
        }
        const QJsonObject properties = entry.toObject().value(u"properties"_s).toObject();
        if (properties.value(u"application.name"_s).toString().compare(applicationName, Qt::CaseInsensitive) == 0) {
            propsOut = properties;
            return true;
        }
    }
    return false;
}

inline bool sinkInputPropertyByApplicationName(const QString &applicationName, const QString &key, QString &valueOut)
{
    QJsonObject props;
    if (!sinkInputPropsByApplicationName(applicationName, props)) {
        return false;
    }
    valueOut = props.value(key).toString();
    return true;
}

}
