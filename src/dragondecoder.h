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

struct AVIOContext;

class DRAGONSDL_EXPORT DragonDecoder : public QObject
{
    Q_OBJECT

public:
    using ReadCallback = std::move_only_function<int(std::span<uint8_t>)>;

    using SamplesCallback = std::move_only_function<void(std::span<const std::float32_t> data, int sampleRate, int nbChannels)>;

    void setSamplesCallback(SamplesCallback cb);

    explicit DragonDecoder(ReadCallback readCb, const QString &filePath = {}, QObject *parent = nullptr);
    ~DragonDecoder() override;

    DragonDecoder(const DragonDecoder &) = delete;
    DragonDecoder &operator=(const DragonDecoder &) = delete;
    DragonDecoder(DragonDecoder &&) = delete;
    DragonDecoder &operator=(DragonDecoder &&) = delete;

    void decodeLoop(std::stop_token st);

    void requestSeek(int64_t positionMs);

    bool hasFatalError() const;

Q_SIGNALS:

    void formatReady(int sampleRate, int nbChannels);

    void durationChanged(int64_t durationMs);

    void streamError(const QString &message);

private:
    ReadCallback m_networkCallback;
    QString m_filePath;
    mutable std::vector<std::float32_t> m_pcmBuffer;

    std::atomic<bool> m_seekRequested{false};
    std::atomic<int64_t> m_seekTargetMs{0};

    SamplesCallback m_samplesCallback;

    struct DecodeSession;

    bool initializeAvio(DecodeSession &session);

    bool openContainer(DecodeSession &session);
    bool findAudioStream(DecodeSession &session);
    bool setupCodec(DecodeSession &session);
    bool setupResampler(DecodeSession &session);
    void emitFormatAndDuration(const DecodeSession &session);
    bool allocatePacketAndFrame(DecodeSession &session);
    bool readAndProcessPacket(DecodeSession &session);
    void drainDecoderFrames(DecodeSession &session, bool canEmitFirstFrame);
    void runMainDecodeLoop(DecodeSession &session, std::stop_token st);
    void flushDecoder(DecodeSession &session);
    void flushResampler(DecodeSession &session);

    std::atomic<bool> m_hadFatalError{false};
};