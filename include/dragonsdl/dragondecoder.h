/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragonsdl_export.h"
#include <stdfloat>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <span>
#include <stop_token>
#include <vector>

class DRAGONSDL_EXPORT DragonDecoder : public QObject
{
    Q_OBJECT

public:
    using ReadCallback = std::move_only_function<int(std::span<uint8_t>)>;

    explicit DragonDecoder(ReadCallback readCb, const QString &filePath = {}, QObject *parent = nullptr);
    ~DragonDecoder() override;

    DragonDecoder(const DragonDecoder &) = delete;
    DragonDecoder &operator=(const DragonDecoder &) = delete;
    DragonDecoder(DragonDecoder &&) = delete;
    DragonDecoder &operator=(DragonDecoder &&) = delete;

    void decodeLoop(std::stop_token st);

    void requestSeek(int64_t positionMs);

Q_SIGNALS:

    void samplesDecoded(std::span<const std::float32_t> data, int sampleRate, int nbChannels);

    void formatReady(int sampleRate, int nbChannels);

    void durationChanged(int64_t durationMs);

    void streamError(const QString &message);

    void stateChanged(bool buffering, double progress);

private:
    ReadCallback m_readCb;
    QString m_filePath;
    mutable std::vector<std::float32_t> m_pcmBuffer;

    std::atomic<bool> m_seekRequested{false};
    std::atomic<int64_t> m_seekTargetMs{0};
};