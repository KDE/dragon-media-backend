/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonstreamfactory.h"

#ifdef DRAGON_HAVE_LIBCDIO
#include "cd/dragoncdstream.h"
#endif

#include "dragonkiostream.h"
#include "dragonradiostream.h"

#include <QUrl>

using namespace Qt::StringLiterals;

bool DragonStreamFactory::isLocalSource(const QUrl &url)
{
    return url.isLocalFile() || url.scheme() == u"content"_s;
}

bool DragonStreamFactory::supportsGaplessPreWarm(const QUrl &url)
{
#ifdef DRAGON_HAVE_LIBCDIO
    if (url.scheme() == u"audiocd"_s) {
        return true;
    }
#endif
    return isLocalSource(url);
}

std::shared_ptr<DragonStream> DragonStreamFactory::createStream(const QUrl &source)
{
    if (isLocalSource(source)) {
        return nullptr;
    }

    const QString scheme = source.scheme();

#ifdef DRAGON_HAVE_LIBCDIO
    if (scheme == u"audiocd"_s) {
        return std::make_shared<DragonCdStream>();
    }
#endif

    if (scheme == u"http"_s || scheme == u"https"_s) {
        return std::make_shared<DragonRadioStream>();
    }

    return std::make_shared<DragonKioStream>();
}
