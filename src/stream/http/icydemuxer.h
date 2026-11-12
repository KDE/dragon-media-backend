/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragonmediabackend_export.h"
#include <dragonicymetadata.h>

#include <QByteArray>
#include <QByteArrayView>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <optional>
#include <utility>

/**
 * Demultiplexes an interleaved ICY stream: audio bytes flow through, and the
 * metadata blocks the station splices in every `icy-metaint` audio bytes are
 * parsed into DragonIcyMetadata and deduplicated.
 *
 * The interleaving is counted across arbitrary packet boundaries, so a metadata
 * block split across deliveries is still read as one.
 */
class DRAGONMEDIABACKEND_EXPORT IcyDemuxer
{
public:
    /** Resets byte counters, pending metadata buffer, and last-emitted metadata. */
    void reset(qint64 metaint = 0);

    /** Sets the metadata interval (in audio bytes) advertised by the `icy-metaint` header. */
    void setIcyMetaInt(qint64 metaint);

    /** Clears pending partial metadata bytes while keeping the last emitted metadata (for reconnects). */
    void clearPendingBytes();

    [[nodiscard]] qint64 icyMetaInt() const noexcept
    {
        return m_metaint;
    }

    /**
     * Parses a single raw ICY metadata block (`StreamTitle='...';StreamUrl='...';\0\0...`).
     */
    [[nodiscard]] static DragonIcyMetadata parseBlock(QByteArrayView metadataBlock);

    /**
     * Parses rawBlock and, when it carries metadata that changed, remembers
     * it and returns it.
     */
    [[nodiscard]] std::optional<DragonIcyMetadata> processBlock(QByteArrayView rawBlock);

    /**
     * Demuxes incoming body bytes into audio chunks and deduplicated DragonIcyMetadata updates.
     */
    template<std::invocable<QByteArrayView> AudioSink, std::invocable<const DragonIcyMetadata &> MetaSink>
    void feed(QByteArrayView data, AudioSink &&onAudio, MetaSink &&onMetadata)
    {
        if (m_metaint <= 0) {
            if (!data.isEmpty()) {
                onAudio(data);
            }
            return;
        }
        feedInterleaved(data, std::forward<AudioSink>(onAudio), std::forward<MetaSink>(onMetadata));
    }

private:
    template<std::invocable<QByteArrayView> AudioSink, std::invocable<const DragonIcyMetadata &> MetaSink>
    void feedInterleaved(QByteArrayView data, AudioSink &&onAudio, MetaSink &&onMetadata)
    {
        QByteArray buffer;
        QByteArrayView rest;
        if (m_pendingData.isEmpty()) {
            rest = data;
        } else {
            buffer = std::move(m_pendingData);
            buffer += data;
            rest = buffer;
        }

        const uint64_t expectedEpoch = m_epoch;

        while (!rest.isEmpty()) {
            const qint64 bytesUntilMeta = m_metaint - m_bytesRead;

            if (bytesUntilMeta > 0) {
                const qsizetype chunkSize = std::min<qsizetype>(rest.size(), bytesUntilMeta);
                const QByteArrayView chunk = rest.first(chunkSize);
                rest = rest.sliced(chunkSize);
                m_bytesRead += chunkSize;
                onAudio(chunk);
                if (m_epoch != expectedEpoch) {
                    return;
                }
                continue;
            }

            // At the boundary one byte counts the metadata that follows, in blocks of
            // sixteen; zero means this interval carried no title.
            const auto lengthByte = static_cast<std::uint8_t>(rest.front());
            const qsizetype metadataLength = lengthByte * 16;

            if (metadataLength > rest.size() - 1) {
                break; // the block continues in the next packet
            }

            rest = rest.sliced(1);
            m_bytesRead = 0;
            if (metadataLength > 0) {
                const auto icy = processBlock(rest.first(metadataLength));
                rest = rest.sliced(metadataLength);
                if (icy) {
                    onMetadata(*icy);
                    if (m_epoch != expectedEpoch) {
                        return;
                    }
                }
            }
        }

        if (m_epoch == expectedEpoch) {
            if (!rest.isEmpty()) {
                m_pendingData = rest.toByteArray();
            } else {
                m_pendingData.clear();
            }
        }
    }

    std::uint64_t m_epoch = 0;
    qint64 m_metaint = 0;
    qint64 m_bytesRead = 0;
    QByteArray m_pendingData;
    DragonIcyMetadata m_lastMetadata;
};
