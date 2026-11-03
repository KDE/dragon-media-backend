/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonhttpbodyframer.h"
#include "dragonhttpprotocol.h"

#include <algorithm>

using namespace Qt::StringLiterals;

void DragonHttpBodyFramer::startFromHeaders(const QHttpHeaders &headers)
{
    using WKH = QHttpHeaders::WellKnownHeader;
    const auto transferEncoding = headers.value(WKH::TransferEncoding);
    const bool chunked = QLatin1StringView(transferEncoding).contains("chunked"_L1, Qt::CaseInsensitive);
    start(chunked, headers.intValue(WKH::ContentLength).value_or(-1));
}

void DragonHttpBodyFramer::start(bool chunked, qint64 contentLength)
{
    m_complete = !chunked && contentLength == 0;
    if (chunked) {
        m_mode = Chunked{};
    } else if (contentLength >= 0) {
        m_mode = FixedLength{.remaining = contentLength};
    } else {
        m_mode = Unframed{};
    }
}

bool DragonHttpBodyFramer::isTruncated() const noexcept
{
    if (m_complete) {
        return false;
    }
    return std::visit(Overloaded{
                          [](const Chunked &) {
                              return true;
                          },
                          [](FixedLength fixed) {
                              return fixed.remaining > 0;
                          },
                          [](Unframed) {
                              return false;
                          },
                      },
                      m_mode);
}

std::expected<QByteArray, DragonHttpBodyFramer::Error> DragonHttpBodyFramer::feed(QByteArrayView data)
{
    if (m_complete || data.isEmpty()) {
        return QByteArray{};
    }

    QByteArray audio;
    const auto res = feed(data, [&](QByteArrayView chunk) {
        audio += chunk;
    });
    if (!res) {
        return std::unexpected(res.error());
    }
    return audio;
}

void DragonHttpBodyFramer::feedFixedLength(FixedLength &state, QByteArrayView data, const std::function<void(QByteArrayView)> &onAudio)
{
    const qsizetype take = std::min<qsizetype>(state.remaining, data.size());
    state.remaining -= take;
    m_complete = (state.remaining == 0);
    if (take > 0) {
        onAudio(data.first(take));
    }
}

static std::expected<std::optional<QByteArrayView>, DragonHttpBodyFramer::Error> takeLine(QByteArrayView &rest)
{
    const qsizetype end = rest.indexOf("\r\n"_ba);
    if (end < 0) {
        if (rest.size() > DragonHttpBodyFramer::MAX_CHUNK_LINE_BYTES) {
            return std::unexpected(DragonHttpBodyFramer::Error::MalformedChunkSize);
        }
        return std::nullopt;
    }
    const QByteArrayView line = rest.first(end);
    rest = rest.sliced(end + 2);
    return line;
}

std::expected<void, DragonHttpBodyFramer::Error>
DragonHttpBodyFramer::feedChunked(Chunked &state, QByteArrayView data, const std::function<void(QByteArrayView)> &onAudio)
{
    QByteArray buffer;
    QByteArrayView rest;
    if (state.pending.isEmpty()) {
        rest = data;
    } else {
        buffer = std::move(state.pending);
        buffer += data;
        rest = buffer;
    }

    while (!rest.isEmpty()) {
        if (state.inTrailer) {
            const auto lineRes = takeLine(rest);
            if (!lineRes) {
                return std::unexpected(lineRes.error());
            }
            if (!*lineRes) {
                break;
            }
            state.trailerBytes += (*lineRes)->size() + 2;
            if (state.trailerBytes > MAX_CHUNK_LINE_BYTES) {
                return std::unexpected(Error::MalformedChunkSize);
            }
            if ((*lineRes)->isEmpty()) {
                m_complete = true;
                break;
            }
            continue;
        }

        if (state.chunkRemaining > 0) {
            const qsizetype take = std::min<qsizetype>(rest.size(), state.chunkRemaining);
            onAudio(rest.first(take));
            rest = rest.sliced(take);
            state.chunkRemaining -= take;
            continue;
        }

        if (rest.startsWith("\r\n"_ba)) {
            rest = rest.sliced(2);
            continue;
        }
        if (rest.startsWith("\r"_ba)) {
            break;
        }

        const auto lineRes = takeLine(rest);
        if (!lineRes) {
            return std::unexpected(lineRes.error());
        }
        if (!*lineRes) {
            break;
        }

        QByteArrayView sizeField = (*lineRes)->trimmed();
        if (const qsizetype extension = sizeField.indexOf(';'); extension >= 0) {
            sizeField = sizeField.first(extension).trimmed();
        }
        const auto size = DragonHttpProtocol::parseNumber<qint64>(sizeField, 16);
        if (!size || *size < 0) {
            return std::unexpected(Error::MalformedChunkSize);
        }
        if (*size == 0) {
            state.inTrailer = true;
            continue;
        }
        state.chunkRemaining = *size;
    }

    if (!rest.isEmpty()) {
        state.pending = rest.toByteArray();
    } else {
        state.pending.clear();
    }

    if (state.pending.size() > MAX_CHUNK_LINE_BYTES) {
        return std::unexpected(Error::MalformedChunkSize);
    }
    return {};
}
