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

namespace PwNodeProps
{

inline bool dumpObjects(QJsonArray &objectsOut)
{
    QProcess pwDump;
    pwDump.start(u"pw-dump"_s, QStringList{});
    if (!pwDump.waitForStarted(2000) || !pwDump.waitForFinished(5000)) {
        return false;
    }

    const QByteArray output = pwDump.readAllStandardOutput();
    const QJsonDocument doc = QJsonDocument::fromJson(output);
    if (!doc.isArray()) {
        return false;
    }

    objectsOut = doc.array();
    return true;
}

inline bool nodePropsByApplicationName(const QString &applicationName, QJsonObject &propsOut)
{
    QJsonArray objects;
    if (!dumpObjects(objects)) {
        return false;
    }

    for (const QJsonValue &value : objects) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        if (object.value(u"type"_s).toString() != u"PipeWire:Interface:Node"_s) {
            continue;
        }
        const QJsonObject props = object.value(u"info"_s).toObject().value(u"props"_s).toObject();
        if (props.value(u"application.name"_s).toString() == applicationName) {
            propsOut = props;
            return true;
        }
    }
    return false;
}

inline bool nodePropertyByApplicationName(const QString &applicationName, const QString &key, QString &valueOut)
{
    QJsonObject props;
    if (!nodePropsByApplicationName(applicationName, props)) {
        return false;
    }
    valueOut = props.value(key).toString();
    return true;
}

}
