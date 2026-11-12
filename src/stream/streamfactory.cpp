/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "streamfactory.h"

#ifdef DRAGON_HAVE_LIBCDIO
#include "cd/cdstream.h"
#endif

#include "http/radiostream.h"
#include "kiostream.h"

#include <QUrl>

using namespace Qt::StringLiterals;

bool StreamFactory::isLocalSource(const QUrl &url)
{
    return url.isLocalFile() || url.scheme() == u"content"_s;
}

bool StreamFactory::supportsGaplessPreWarm(const QUrl &url)
{
#ifdef DRAGON_HAVE_LIBCDIO
    if (url.scheme() == u"audiocd"_s) {
        return true;
    }
#endif
    return isLocalSource(url);
}

std::shared_ptr<Stream> StreamFactory::createStream(const QUrl &source)
{
    if (isLocalSource(source)) {
        return nullptr;
    }

    const QString scheme = source.scheme();

#ifdef DRAGON_HAVE_LIBCDIO
    if (scheme == u"audiocd"_s) {
        return std::make_shared<CdStream>();
    }
#endif

    if (scheme == u"http"_s || scheme == u"https"_s) {
        return std::make_shared<RadioStream>();
    }

    return std::make_shared<KioStream>();
}
