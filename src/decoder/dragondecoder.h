/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#pragma once

#include "dragoncompletion.h"
#include "dragoncppcompat.h"
#include "dragonmediabackend_export.h"
#include "player/dragonevent.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

class DRAGONMEDIABACKEND_EXPORT DragonDecoder : public QObject
{
    Q_OBJECT

public:
    using ReadCallback = dragon::compat::move_only_function<int(std::span<uint8_t>)>;

    enum class SeekWhence {
        Set,
        Cur,
        End
    };

    using SeekCallback = std::function<qint64(qint64 offset, SeekWhence whence)>;

    explicit DragonDecoder(ReadCallback readCb, SeekCallback seekCb = nullptr, qint64 streamSize = -1, const QString &filePath = {}, QObject *parent = nullptr);
    ~DragonDecoder() override;

    DragonDecoder(const DragonDecoder &) = delete;
    DragonDecoder &operator=(const DragonDecoder &) = delete;
    DragonDecoder(DragonDecoder &&) = delete;
    DragonDecoder &operator=(DragonDecoder &&) = delete;

    DragonMediaBackend::InitResult initialize();

    dragon::compat::generator<DragonMediaBackend::DecodeEvent> decodeLoop(std::stop_token st);

    void requestSeek(qint64 positionMs);

    bool hasFatalError() const;

Q_SIGNALS:

    void streamError(const QString &message);

private:
    ReadCallback m_networkCallback;
    SeekCallback m_seekCallback;
    qint64 m_streamSize{-1};
    QString m_filePath;

    std::atomic<bool> m_seekRequested{false};
    std::atomic<qint64> m_seekTargetMs{0};

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

    std::optional<DragonMediaBackend::SamplesChunk> drainDecoderFrames(DecodeSession &session);
    void flushDecoder(DecodeSession &session);
    std::optional<DragonMediaBackend::SamplesChunk> flushResampler(DecodeSession &session);

    // Resamples up to maxOutSamples from the given input (nullptr to drain the
    // resampler) into a pooled buffer and wraps it in a self-owning chunk.
    std::optional<DragonMediaBackend::SamplesChunk> resampleInto(DecodeSession &session, const uint8_t *const *in, int inSamples, int maxOutSamples);

    std::atomic<bool> m_hadFatalError{false};
};
