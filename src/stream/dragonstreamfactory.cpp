/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonstreamfactory.h"

#include "dragonkiostream.h"
#include "dragonradiostream.h"

#include <QUrl>

using namespace Qt::StringLiterals;

bool DragonStreamFactory::isLocalSource(const QUrl &url)
{
    return url.isLocalFile() || url.scheme() == u"content"_s;
}

std::shared_ptr<DragonStream> DragonStreamFactory::createStream(const QUrl &source)
{
    if (isLocalSource(source)) {
        return nullptr;
    }

    const QString scheme = source.scheme();

    if (scheme == u"http"_s || scheme == u"https"_s) {
        return std::make_shared<DragonRadioStream>();
    }

    return std::make_shared<DragonKioStream>();
}
