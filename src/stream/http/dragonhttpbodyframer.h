/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include "dragonoverloaded.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QHttpHeaders>

#include <concepts>
#include <expected>
#include <functional>
#include <variant>

/**
 * Unwraps the framing of a response body into the audio bytes it carries.
 *
 * A body is framed either by a Content-Length to count down, by chunked
 * encoding, or by nothing at all, in which case it runs until the server hangs
 * up. Bytes are fed in as the transport delivers them, and the audio they
 * carry comes out whole and in order whatever the packet boundaries were.
 *
 * The three framing rules cannot bleed into each other: each is its own state,
 * and isTruncated() asks the active one whether it was still owed bytes
 * when the transport ended.
 */
class DRAGONMEDIABACKEND_EXPORT DragonHttpBodyFramer
{
public:
    enum class Error {
        MalformedChunkSize,
    };

    // Initializes framing from response headers (Transfer-Encoding: chunked vs Content-Length vs unframed).
    void startFromHeaders(const QHttpHeaders &headers);

    void start(bool chunked, qint64 contentLength);

    template<std::invocable<QByteArrayView> AudioSink>
    std::expected<void, Error> feed(QByteArrayView data, AudioSink &&onAudio)
    {
        if (m_complete || data.isEmpty()) {
            return {};
        }

        return std::visit(Overloaded{
                              [&](Unframed &) -> std::expected<void, Error> {
                                  onAudio(data);
                                  return {};
                              },
                              [&](FixedLength &fixed) -> std::expected<void, Error> {
                                  feedFixedLength(fixed, data, onAudio);
                                  return {};
                              },
                              [&](Chunked &chunked) -> std::expected<void, Error> {
                                  return feedChunked(chunked, data, onAudio);
                              },
                          },
                          m_mode);
    }

    // Consumes data and returns the audio bytes it carries, or Error::MalformedChunkSize.
    [[nodiscard]] std::expected<QByteArray, Error> feed(QByteArrayView data);

    // Resets the framer to default unframed state.
    void reset()
    {
        start(false, -1);
    }

    // Whether the framing itself has said the body is complete.
    [[nodiscard]] bool isComplete() const noexcept
    {
        return m_complete;
    }

    /**
     * Whether the framing rules were still owed bytes when the transport
     * ended: a body without framing ends at the hang-up, but a declared
     * length or a chunked body cut short was truncated.
     */
    [[nodiscard]] bool isTruncated() const noexcept;

    static constexpr qsizetype MAX_CHUNK_LINE_BYTES = 64 * 1024;

private:
    struct Unframed { };

    struct FixedLength {
        qint64 remaining = 0;
    };

    struct Chunked {
        QByteArray pending;
        qint64 chunkRemaining = 0;
        qsizetype trailerBytes = 0;
        bool inTrailer = false;
    };

    [[nodiscard]] std::expected<void, Error> feedChunked(Chunked &state, QByteArrayView data, const std::function<void(QByteArrayView)> &onAudio);
    void feedFixedLength(FixedLength &state, QByteArrayView data, const std::function<void(QByteArrayView)> &onAudio);

    std::variant<Unframed, FixedLength, Chunked> m_mode{Unframed{}};
    bool m_complete = false;
};
