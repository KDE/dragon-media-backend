/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragoncdurl.h"

#include <QUrlQuery>

#include <algorithm>
#include <ranges>
#include <vector>

using namespace Qt::StringLiterals;

namespace
{

/*! The \c track query item, when present and numeric. */
[[nodiscard]] std::optional<int> trackFromUrl(const QUrl &url)
{
    const QUrlQuery query(url);
    if (!query.hasQueryItem(u"track"_s)) {
        return std::nullopt;
    }
    bool ok = false;
    const int track = query.queryItemValue(u"track"_s, QUrl::FullyDecoded).toInt(&ok);
    return ok ? std::optional<int>{track} : std::nullopt;
}

}

QUrl DragonCdUrl::makeTrackUrl(int trackNumber, const QString &device)
{
    QUrl url;
    url.setScheme(u"audiocd"_s);
    url.setPath(u"/Track %1.wav"_s.arg(trackNumber, 2, 10, QChar(u'0')));
    QUrlQuery query;
    query.addQueryItem(u"track"_s, QString::number(trackNumber));
    if (!device.isEmpty()) {
        query.addQueryItem(u"device"_s, device);
    }
    url.setQuery(query);
    return url;
}

QString DragonCdUrl::deviceFromUrl(const QUrl &url)
{
    const QUrlQuery query(url);
    return query.hasQueryItem(u"device"_s) ? query.queryItemValue(u"device"_s, QUrl::FullyDecoded) : QString();
}

std::optional<int> DragonCdUrl::trackNumberFromName(const QString &fileName, int firstTrackNum, int lastTrackNum)
{
    QString name = fileName;
    const qsizetype dot = name.lastIndexOf(u'.');
    if (dot >= 0) {
        name.truncate(dot);
    }

    struct Run {
        int value = 0;
        qsizetype length = 0;
    };
    std::vector<Run> runs;

    qsizetype i = 0;
    while (i < name.size()) {
        if (!name[i].isDigit()) {
            ++i;
            continue;
        }
        qsizetype start = i;
        while (i < name.size() && name[i].isDigit()) {
            ++i;
        }
        bool ok = false;
        const int value = name.mid(start, i - start).toInt(&ok);
        if (ok) {
            runs.push_back(Run{value, i - start});
        }
    }

    auto validRuns = runs | std::views::filter([firstTrackNum, lastTrackNum](const Run &r) {
                         return r.value >= firstTrackNum && r.value <= lastTrackNum;
                     });

    if (auto it = std::ranges::find_if(validRuns,
                                       [](const Run &r) {
                                           return r.length == 2;
                                       });
        it != std::ranges::end(validRuns)) {
        return it->value;
    }
    if (auto it = std::ranges::begin(validRuns); it != std::ranges::end(validRuns)) {
        return it->value;
    }
    return std::nullopt;
}

std::optional<int> DragonCdUrl::resolveTrackNumber(const QUrl &url, int firstTrackNum, int lastTrackNum)
{
    if (const std::optional<int> track = trackFromUrl(url)) {
        return track;
    }
    return trackNumberFromName(url.fileName(), firstTrackNum, lastTrackNum);
}
