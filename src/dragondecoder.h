/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "dragoncompletion.h"
#include "dragonevent.h"
#include "dragonsdl_export.h"
#include <stdfloat>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <generator>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class DRAGONSDL_EXPORT DragonDecoder : public QObject
{
    Q_OBJECT

public:
    using ReadCallback = std::move_only_function<int(std::span<uint8_t>)>;

    enum class SeekWhence {
        Set,
        Cur,
        End,
        Size
    };

    using SeekCallback = std::function<int64_t(int64_t offset, SeekWhence whence)>;

    explicit DragonDecoder(ReadCallback readCb, SeekCallback seekCb = nullptr, const QString &filePath = {}, QObject *parent = nullptr);
    ~DragonDecoder() override;

    DragonDecoder(const DragonDecoder &) = delete;
    DragonDecoder &operator=(const DragonDecoder &) = delete;
    DragonDecoder(DragonDecoder &&) = delete;
    DragonDecoder &operator=(DragonDecoder &&) = delete;

    DragonSdl::InitResult initialize();

    std::generator<DragonSdl::DecodeEvent> decodeLoop(std::stop_token st);

    void requestSeek(int64_t positionMs);

    bool hasFatalError() const;

Q_SIGNALS:

    void streamError(const QString &message);

private:
    ReadCallback m_networkCallback;
    SeekCallback m_seekCallback;
    QString m_filePath;
    std::vector<std::float32_t> m_pcmBuffer;
    std::vector<std::float32_t> m_pendingSamples;

    std::atomic<bool> m_seekRequested{false};
    std::atomic<int64_t> m_seekTargetMs{0};

    struct DecodeSession;
    std::unique_ptr<DecodeSession> m_session;

    bool initializeAvio(DecodeSession &session);
    bool openContainer(DecodeSession &session);
    bool findAudioStream(DecodeSession &session);
    bool setupCodec(DecodeSession &session);
    bool setupResampler(DecodeSession &session);
    bool allocatePacketAndFrame(DecodeSession &session);
    bool readAndProcessPacket(DecodeSession &session);

    bool isRecoverableReadError(int errorCode) const;

    QString avErrorString(int errorCode) const;

    std::optional<DragonSdl::SamplesChunk> drainDecoderFrames(DecodeSession &session);

    void flushDecoder(DecodeSession &session);
    std::optional<DragonSdl::SamplesChunk> flushResampler(DecodeSession &session);
    std::atomic<bool> m_hadFatalError{false};
};
