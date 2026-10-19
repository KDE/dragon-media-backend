/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonhttpprotocol.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QNetworkProxyFactory>
#include <QStringBuilder>
#include <dragonmediabackend_version.h>

#include <algorithm>
#include <ranges>

using namespace Qt::StringLiterals;

namespace DragonHttpProtocol
{

const QByteArray &userAgent()
{
    static const QByteArray agent = []() -> QByteArray {
        const QString appName = QCoreApplication::applicationName();
        if (appName.isEmpty()) {
            return "DragonMediaBackend/" DRAGONMEDIABACKEND_VERSION_STRING ""_ba;
        }
        return "DragonMediaBackend/" DRAGONMEDIABACKEND_VERSION_STRING " ("_ba + appName.toUtf8() + ")"_ba;
    }();
    return agent;
}

bool isHttps(const QUrl &url) noexcept
{
    return url.scheme() == u"https"_s;
}

int schemeDefaultPort(const QUrl &url) noexcept
{
    return isHttps(url) ? 443 : 80;
}

int connectPort(const QUrl &url) noexcept
{
    return url.port(schemeDefaultPort(url));
}

QByteArray requestTarget(const QUrl &url)
{
    QByteArray target = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority | QUrl::RemoveFragment);
    if (target.isEmpty() || target.startsWith('?')) {
        target.prepend('/');
    }
    return target;
}

QByteArray hostHeader(const QUrl &url)
{
    QUrl u = url;
    if (u.port() == schemeDefaultPort(u)) {
        u.setPort(-1);
    }
    return u.authority(QUrl::RemoveUserInfo | QUrl::FullyEncoded).toUtf8();
}

bool isForwardingProxy(const QNetworkProxy &proxy) noexcept
{
    return proxy.type() == QNetworkProxy::HttpProxy || proxy.type() == QNetworkProxy::HttpCachingProxy;
}

QNetworkProxy resolveProxy(const QUrl &url)
{
    const bool https = isHttps(url);
    const auto usable = [https](const QNetworkProxy &proxy) {
        return proxy.capabilities().testFlag(QNetworkProxy::TunnelingCapability) || proxy.type() == QNetworkProxy::Socks5Proxy
            || (!https && isForwardingProxy(proxy));
    };
    const auto proxies = QNetworkProxyFactory::proxyForQuery(QNetworkProxyQuery(url));
    const auto it = std::ranges::find_if(proxies, usable);
    if (it == proxies.end()) {
        return QNetworkProxy(QNetworkProxy::NoProxy);
    }

    QNetworkProxy resolved = *it;
    if (resolved.type() == QNetworkProxy::Socks5Proxy && !resolved.capabilities().testFlag(QNetworkProxy::TunnelingCapability)) {
        // work around a bug in Qt, I think. doesn't identify "socks5h://" as tunneling
        resolved.setCapabilities(resolved.capabilities() | QNetworkProxy::TunnelingCapability);
    }
    return resolved;
}

HttpRequest::HttpRequest(QByteArray target, QByteArrayView host)
    : m_target(std::move(target))
{
    setHeader("Host"_ba, host);
}

void HttpRequest::setHeader(QByteArrayView name, QByteArrayView value)
{
    const QByteArray nameBa = name.toByteArray();
    const auto it = std::ranges::find_if(m_headers, [&](const auto &pair) {
        return pair.first.compare(nameBa, Qt::CaseInsensitive) == 0;
    });
    if (it != m_headers.end()) {
        it->second = value.toByteArray();
    } else {
        m_headers.append({nameBa, value.toByteArray()});
    }
}

void HttpRequest::clearHeaders()
{
    m_headers.clear();
}

QByteArray HttpRequest::toByteArray() const
{
    QByteArray request;
    request.reserve(256 + m_target.size());
    request += "GET "_ba % m_target % " HTTP/1.1\r\n"_ba;
    for (const auto &[name, value] : m_headers) {
        request += name % ": "_ba % value % "\r\n"_ba;
    }
    request += "\r\n"_ba;
    return request;
}

QByteArray buildStreamGetRequest(const QUrl &url, const QNetworkProxy &forwardingProxy, std::optional<qint64> rangeStart)
{
    QByteArray target = isForwardingProxy(forwardingProxy) ? url.toString(QUrl::RemoveFragment | QUrl::FullyEncoded).toUtf8() : requestTarget(url);
    HttpRequest request(std::move(target), hostHeader(url));

    if (isForwardingProxy(forwardingProxy) && !forwardingProxy.user().isEmpty()) {
        const QByteArray credentials = (forwardingProxy.user() + u':' + forwardingProxy.password()).toUtf8().toBase64();
        request.setHeader("Proxy-Authorization"_ba, QByteArray{"Basic "_ba + credentials});
    }

    request.setHeader("User-Agent"_ba, userAgent());
    request.setHeader("Icy-Metadata"_ba, "1"_ba);
    if (rangeStart) {
        request.setHeader("Range"_ba, QByteArray{"bytes="_ba + QByteArray::number(*rangeStart) + "-"_ba});
    }

    return request.toByteArray();
}

static std::expected<int, IcyParseFailure> parseStatusLine(QByteArrayView rawStatusLine)
{
    const QByteArray line = rawStatusLine.trimmed().toByteArray();
    const auto parts = line.simplified().split(' ');
    if (parts.size() < 2) {
        return std::unexpected(IcyParseFailure{IcyParseError::MalformedStatusLine, line});
    }
    // the following line is why we can't use QNAM and have to implement HTTP :(
    if (parts[0] != "ICY"_ba && !parts[0].startsWith("HTTP/"_ba)) {
        return std::unexpected(IcyParseFailure{IcyParseError::NotAudioResponse, line});
    }
    const auto statusCode = parseNumber<int>(parts[1]);
    if (!statusCode) {
        return std::unexpected(IcyParseFailure{IcyParseError::NonNumericStatus, line});
    }
    return *statusCode;
}

std::expected<IcyResponse, IcyParseFailure> parseIcyResponse(QByteArrayView headerBlock)
{
    if (headerBlock.isEmpty()) {
        return std::unexpected(IcyParseFailure{IcyParseError::MalformedStatusLine, {}});
    }

    auto lines = headerBlock | std::views::split('\n');

    const auto statusCode = parseStatusLine(*lines.begin());
    if (!statusCode) {
        return std::unexpected(statusCode.error());
    }

    IcyResponse response{.statusCode = *statusCode, .headers = {}};
    for (auto rawLine : lines | std::views::drop(1)) {
        const QByteArrayView line = QByteArrayView(rawLine).trimmed();
        if (const qsizetype separator = line.indexOf(':'); separator > 0) {
            response.headers.append(line.first(separator), line.sliced(separator + 1).trimmed());
        }
    }
    return response;
}

std::optional<ContentRange> parseContentRange(QByteArrayView headerValue) noexcept
{
    const QByteArray clean = headerValue.toByteArray().simplified();
    const auto unitAndSpec = clean.split(' ');
    if (unitAndSpec.size() != 2 || unitAndSpec[0].compare("bytes"_ba, Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }

    const auto specParts = unitAndSpec[1].split('/');
    if (specParts.size() != 2) {
        return std::nullopt;
    }

    const auto rangeParts = specParts[0].split('-');
    if (rangeParts.size() != 2) {
        return std::nullopt;
    }

    const auto first = parseNumber<qint64>(rangeParts[0]);
    const auto last = parseNumber<qint64>(rangeParts[1]);
    if (!first || !last || *first < 0 || *last < *first) {
        return std::nullopt;
    }

    qint64 total = -1;
    if (specParts[1] != "*"_ba) {
        const auto parsedTotal = parseNumber<qint64>(specParts[1]);
        if (!parsedTotal || *parsedTotal < 0) {
            return std::nullopt;
        }
        total = *parsedTotal;
    }

    return ContentRange{.first = *first, .last = *last, .total = total};
}

QString IcyParseFailure::message() const
{
    return i18n("Malformed response");
}

} // namespace DragonHttpProtocol
