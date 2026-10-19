/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include "dragonoverloaded.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QByteArrayView>
#include <QHttpHeaders>
#include <QList>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QString>
#include <QUrl>

#include <charconv>
#include <concepts>
#include <expected>
#include <optional>
#include <utility>

namespace DragonHttpProtocol
{

using ::Overloaded;

template<std::integral T>
[[nodiscard]] constexpr std::optional<T> parseNumber(QByteArrayView s, int base = 10) noexcept
{
    if (s.isEmpty()) {
        return std::nullopt;
    }
    T v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    return (ec == std::errc{} && p == s.data() + s.size()) ? std::optional{v} : std::nullopt;
}

struct Failure {
    QNetworkReply::NetworkError error = QNetworkReply::ProtocolFailure;
    QString message;

    bool operator==(const Failure &) const = default;
};

struct IcyResponse {
    int statusCode = 0;
    QHttpHeaders headers;
};

enum class IcyParseError {
    MalformedStatusLine,
    NotAudioResponse,
    NonNumericStatus,
};

struct IcyParseFailure {
    IcyParseError error = IcyParseError::MalformedStatusLine;
    QByteArray statusLine;

    [[nodiscard]] DRAGONMEDIABACKEND_EXPORT QString message() const;
};

class DRAGONMEDIABACKEND_EXPORT HttpRequest
{
public:
    HttpRequest(QByteArray target, QByteArrayView host);

    void setHeader(QByteArrayView name, QByteArrayView value);
    void clearHeaders();

    [[nodiscard]] QByteArray toByteArray() const;

private:
    QByteArray m_target;
    QList<std::pair<QByteArray, QByteArray>> m_headers;
};

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT const QByteArray &userAgent();
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT bool isHttps(const QUrl &url) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT int schemeDefaultPort(const QUrl &url) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT int connectPort(const QUrl &url) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QByteArray requestTarget(const QUrl &url);
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QByteArray hostHeader(const QUrl &url);
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT bool isForwardingProxy(const QNetworkProxy &proxy) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QNetworkProxy resolveProxy(const QUrl &url);

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT QByteArray buildStreamGetRequest(const QUrl &url,
                                                                         const QNetworkProxy &forwardingProxy,
                                                                         std::optional<qint64> rangeStart);

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT std::expected<IcyResponse, IcyParseFailure> parseIcyResponse(QByteArrayView headerBlock);

struct ContentRange {
    qint64 first = -1;
    qint64 last = -1;
    qint64 total = -1;
};

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT std::optional<ContentRange> parseContentRange(QByteArrayView headerValue) noexcept;

[[nodiscard]] DRAGONMEDIABACKEND_EXPORT constexpr QNetworkReply::NetworkError errorForStatus(int statusCode) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT constexpr QNetworkReply::NetworkError mapSocketError(QAbstractSocket::SocketError socketError) noexcept;
[[nodiscard]] DRAGONMEDIABACKEND_EXPORT constexpr bool isRetryable(QNetworkReply::NetworkError error) noexcept;

constexpr QNetworkReply::NetworkError errorForStatus(int statusCode) noexcept
{
    switch (statusCode) {
    case 400:
    case 418:
        return QNetworkReply::ProtocolInvalidOperationError;
    case 401:
        return QNetworkReply::AuthenticationRequiredError;
    case 403:
        return QNetworkReply::ContentAccessDenied;
    case 404:
        return QNetworkReply::ContentNotFoundError;
    case 405:
        return QNetworkReply::ContentOperationNotPermittedError;
    case 407:
        return QNetworkReply::ProxyAuthenticationRequiredError;
    case 409:
        return QNetworkReply::ContentConflictError;
    case 410:
        return QNetworkReply::ContentGoneError;
    case 500:
        return QNetworkReply::InternalServerError;
    case 501:
        return QNetworkReply::OperationNotImplementedError;
    case 503:
        return QNetworkReply::ServiceUnavailableError;
    default:
        break;
    }
    if (statusCode > 500) {
        return QNetworkReply::UnknownServerError;
    }
    if (statusCode >= 400) {
        return QNetworkReply::UnknownContentError;
    }
    return QNetworkReply::ProtocolFailure;
}

constexpr QNetworkReply::NetworkError mapSocketError(QAbstractSocket::SocketError socketError) noexcept
{
    switch (socketError) {
    case QAbstractSocket::ConnectionRefusedError:
        return QNetworkReply::ConnectionRefusedError;
    case QAbstractSocket::RemoteHostClosedError:
        return QNetworkReply::RemoteHostClosedError;
    case QAbstractSocket::HostNotFoundError:
        return QNetworkReply::HostNotFoundError;
    case QAbstractSocket::SocketTimeoutError:
        return QNetworkReply::TimeoutError;
    case QAbstractSocket::ProxyAuthenticationRequiredError:
        return QNetworkReply::ProxyAuthenticationRequiredError;
    case QAbstractSocket::SslHandshakeFailedError:
    case QAbstractSocket::SslInternalError:
    case QAbstractSocket::SslInvalidUserDataError:
        return QNetworkReply::SslHandshakeFailedError;
    case QAbstractSocket::ProxyConnectionRefusedError:
        return QNetworkReply::ProxyConnectionRefusedError;
    case QAbstractSocket::ProxyConnectionClosedError:
        return QNetworkReply::ProxyConnectionClosedError;
    case QAbstractSocket::ProxyConnectionTimeoutError:
        return QNetworkReply::ProxyTimeoutError;
    case QAbstractSocket::ProxyNotFoundError:
        return QNetworkReply::ProxyNotFoundError;
    case QAbstractSocket::ProxyProtocolError:
        return QNetworkReply::UnknownProxyError;
    case QAbstractSocket::UnsupportedSocketOperationError:
        return QNetworkReply::ProtocolInvalidOperationError;
    case QAbstractSocket::TemporaryError:
        return QNetworkReply::TemporaryNetworkFailureError;
    default:
        return QNetworkReply::UnknownNetworkError;
    }
}

constexpr bool isRetryable(QNetworkReply::NetworkError error) noexcept
{
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ServiceUnavailableError:
        return true;
    default:
        return false;
    }
}

} // namespace DragonHttpProtocol
