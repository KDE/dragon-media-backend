/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragondecoder.h>
#include <stdfloat>

#include "dragonsdl_decoder_logging.h"
#include <QScopeGuard>

#include <cstring>
#include <expected>
#include <memory>
#include <ranges>
#include <span>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
}
#endif

using namespace Qt::StringLiterals;

static constexpr int IO_BUFFER_SIZE = 65536;

struct AvPacketDeleter {
    void operator()(AVPacket *pkt) const noexcept
    {
        if (pkt) {
            av_packet_free(&pkt);
        }
    }
};

struct AvFrameDeleter {
    void operator()(AVFrame *frame) const noexcept
    {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

struct AvFormatCtxDeleter {
    void operator()(AVFormatContext *ctx) const noexcept
    {
        if (!ctx) {
            return;
        }
        const bool customIo = ctx->flags & AVFMT_FLAG_CUSTOM_IO;
        AVIOContext *pb = customIo ? ctx->pb : nullptr;
        uint8_t *buffer = nullptr;
        if (customIo && pb) {
            buffer = pb->buffer;
            ctx->pb = nullptr;
        }
        avformat_close_input(&ctx);
        if (customIo && pb) {
            avio_context_free(&pb);
            av_free(buffer);
        }
    }
};

struct AvCodecCtxDeleter {
    void operator()(AVCodecContext *ctx) const noexcept
    {
        if (ctx) {
            avcodec_free_context(&ctx);
        }
    }
};

using CodecContextPtr = std::unique_ptr<AVCodecContext, AvCodecCtxDeleter>;

struct SwrCtxDeleter {
    void operator()(SwrContext *ctx) const noexcept
    {
        if (ctx) {
            swr_free(&ctx);
        }
    }
};

struct DragonDecoder::DecodeSession {
    std::unique_ptr<AVFormatContext, AvFormatCtxDeleter> fmtCtx;
    CodecContextPtr codecCtx;
    std::unique_ptr<SwrContext, SwrCtxDeleter> swrCtx;
    const AVCodec *codec = nullptr;
    AVStream *audioStream = nullptr;
    int audioStreamIndex = -1;
    int sampleRate = 0;
    int nbChannels = 0;
    std::unique_ptr<AVPacket, AvPacketDeleter> pkt;
    std::unique_ptr<AVFrame, AvFrameDeleter> frame;
    bool firstFrame = true;
    int packetCount = 0;
    int frameCount = 0;
};

struct DragonDecoder::AvioContextHandle {
    AVIOContext *ctx = nullptr;

    AvioContextHandle() = default;
    AvioContextHandle(AVIOContext *c)
        : ctx(c)
    {
    }
    ~AvioContextHandle()
    {
        if (ctx) {
            uint8_t *buf = ctx->buffer;
            avio_context_free(&ctx);
            av_free(buf);
        }
    }
    AvioContextHandle(AvioContextHandle &&other) noexcept
        : ctx(other.ctx)
    {
        other.ctx = nullptr;
    }
    AvioContextHandle(const AvioContextHandle &) = delete;
    AvioContextHandle &operator=(const AvioContextHandle &) = delete;

    explicit operator bool() const noexcept
    {
        return ctx != nullptr;
    }
    AVIOContext *release() noexcept
    {
        auto *tmp = ctx;
        ctx = nullptr;
        return tmp;
    }
};

struct DragonDecoder::AvioInitResult {
    bool ok = false;
    AvioContextHandle handle;

    AvioInitResult() = default;
    AvioInitResult(bool success, AvioContextHandle h)
        : ok(success)
        , handle(std::move(h))
    {
    }
};

DragonDecoder::DragonDecoder(ReadCallback readCb, const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_networkCallback(std::move(readCb))
    , m_filePath(filePath)
{
}

DragonDecoder::~DragonDecoder() = default;

void DragonDecoder::decodeLoop(std::stop_token st)
{
    av_log_set_level(AV_LOG_ERROR);

    DecodeSession session;

    auto avio = initializeAvio();
    if (!avio) {
        return;
    }
    if (!openContainer(std::move(*avio), session)) {
        return;
    }
    if (!findAudioStream(session)) {
        return;
    }
    if (!setupCodec(session)) {
        return;
    }
    if (!setupResampler(session)) {
        return;
    }

    emitFormatAndDuration(session);

    if (!allocatePacketAndFrame(session)) {
        return;
    }

    runMainDecodeLoop(session, st);
    flushDecoder(session);
    flushResampler(session);

    qCDebug(dragonsdlDecoder) << "decodeLoop finished total packets=" << session.packetCount << "total frames=" << session.frameCount;
}

enum class AvioError {
    AllocateFailed,
    CreateFailed
};
std::expected<DragonDecoder::AvioContextHandle, AvioError> DragonDecoder::initializeAvio()
{
    if (!m_networkCallback || !m_filePath.isEmpty()) {
        return AvioContextHandle{};
    }

    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(IO_BUFFER_SIZE));
    if (!ioBuffer) {
        Q_EMIT streamError(u"Failed to allocate AVIOContext buffer"_s);
        return std::unexpected(AvioError::AllocateFailed);
    }

    auto readPacket = [](void *opaque, uint8_t *buf, int bufSize) -> int {
        auto *self = static_cast<DragonDecoder *>(opaque);
        const int ret = self->m_networkCallback(std::span(buf, static_cast<size_t>(bufSize)));
        return ret == 0 ? AVERROR_EOF : ret;
    };

    AVIOContext *avioCtx = avio_alloc_context(ioBuffer, IO_BUFFER_SIZE, 0, this, readPacket, nullptr, nullptr);
    if (!avioCtx) {
        av_free(ioBuffer);
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"Failed to create AVIOContext"_s);
        return std::unexpected(AvioError::CreateFailed);
    }

    return AvioContextHandle(avioCtx);
}

bool DragonDecoder::openContainer(AvioContextHandle handle, DecodeSession &session)
{
    AVFormatContext *rawFmtCtx = nullptr;

    if (m_networkCallback) {
        rawFmtCtx = avformat_alloc_context();
        if (!rawFmtCtx) {
            return false;
        }
        rawFmtCtx->pb = handle.ctx;

        if (const int err = avformat_open_input(&rawFmtCtx, "", nullptr, nullptr); err < 0) {
            if (rawFmtCtx) {
                rawFmtCtx->pb = nullptr;
                avformat_free_context(rawFmtCtx);
            }
            m_hadFatalError.store(true, std::memory_order_relaxed);
            Q_EMIT streamError(u"avformat_open_input failed for network stream"_s);
            return false;
        }
    } else {
        if (const int err = avformat_open_input(&rawFmtCtx, m_filePath.toUtf8().constData(), nullptr, nullptr); err < 0) {
            m_hadFatalError.store(true, std::memory_order_relaxed);
            Q_EMIT streamError(u"avformat_open_input failed for %1"_s.arg(m_filePath));
            return false;
        }
    }

    handle.release();
    session.fmtCtx.reset(rawFmtCtx);
    return true;
}

bool DragonDecoder::findAudioStream(DecodeSession &session)
{
    if (const int err = avformat_find_stream_info(session.fmtCtx.get(), nullptr); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"avformat_find_stream_info failed"_s);
        return false;
    }

    for (auto [i, stream] : std::views::enumerate(std::span{session.fmtCtx->streams, session.fmtCtx->nb_streams})) {
        if (const AVCodecParameters *par = stream->codecpar; par->codec_type == AVMEDIA_TYPE_AUDIO) {
            session.codec = avcodec_find_decoder(par->codec_id);
            if (session.codec) {
                session.audioStreamIndex = static_cast<int>(i);
                session.audioStream = stream;
                break;
            }
        }
    }

    if (session.audioStreamIndex < 0 || !session.codec) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"No supported audio stream found"_s);
        return false;
    }

    return true;
}

bool DragonDecoder::setupCodec(DecodeSession &session)
{
    CodecContextPtr codecCtx{avcodec_alloc_context3(session.codec)};
    if (!codecCtx) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"Failed to allocate codec context"_s);
        return false;
    }

    if (const int err = avcodec_parameters_to_context(codecCtx.get(), session.audioStream->codecpar); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"avcodec_parameters_to_context failed"_s);
        return false;
    }

    if (const int err = avcodec_open2(codecCtx.get(), session.codec, nullptr); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"avcodec_open2 failed"_s);
        return false;
    }

    session.codecCtx.reset(codecCtx.release());
    session.sampleRate = session.codecCtx->sample_rate;
    session.nbChannels = session.codecCtx->ch_layout.nb_channels;
    return true;
}

bool DragonDecoder::setupResampler(DecodeSession &session)
{
    SwrContext *rawSwrCtx = nullptr;
    const AVChannelLayout outLayout = session.codecCtx->ch_layout;
    int ret = swr_alloc_set_opts2(&rawSwrCtx,
                                  &outLayout,
                                  AV_SAMPLE_FMT_FLT,
                                  session.sampleRate,
                                  &session.codecCtx->ch_layout,
                                  session.codecCtx->sample_fmt,
                                  session.sampleRate,
                                  0,
                                  nullptr);
    if (ret < 0 || !rawSwrCtx) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"swr_alloc_set_opts2 failed"_s);
        return false;
    }

    session.swrCtx.reset(rawSwrCtx);
    ret = swr_init(session.swrCtx.get());
    if (ret < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"swr_init failed"_s);
        return false;
    }

    return true;
}

void DragonDecoder::emitFormatAndDuration(const DecodeSession &session)
{
    qCDebug(dragonsdlDecoder) << "formatReady sr=" << session.sampleRate << "ch=" << session.nbChannels << "codec=" << session.codec->name;
    Q_EMIT formatReady(session.sampleRate, session.nbChannels);

    if (session.fmtCtx->duration != AV_NOPTS_VALUE) {
        const int64_t durationMs = session.fmtCtx->duration / (AV_TIME_BASE / 1000);
        qCDebug(dragonsdlDecoder) << "duration=" << durationMs << "ms";
        Q_EMIT durationChanged(durationMs);
    } else {
        qCDebug(dragonsdlDecoder) << "duration unknown";
    }
}

bool DragonDecoder::allocatePacketAndFrame(DecodeSession &session)
{
    session.pkt.reset(av_packet_alloc());
    session.frame.reset(av_frame_alloc());
    if (!session.pkt || !session.frame) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        Q_EMIT streamError(u"Failed to allocate packet/frame"_s);
        return false;
    }
    return true;
}

bool DragonDecoder::readAndProcessPacket(DecodeSession &session)
{
    if (m_seekRequested.exchange(false, std::memory_order_acq_rel)) {
        int64_t targetMs = m_seekTargetMs.load(std::memory_order_relaxed);
        AVRational msTimeBase = AVRational{1, 1000};
        int64_t streamTimestamp = av_rescale_q(targetMs, msTimeBase, session.audioStream->time_base);
        int seekRet = av_seek_frame(session.fmtCtx.get(), session.audioStreamIndex, streamTimestamp, AVSEEK_FLAG_BACKWARD);
        if (seekRet >= 0) {
            avcodec_flush_buffers(session.codecCtx.get());
            swr_convert(session.swrCtx.get(), nullptr, 0, nullptr, 0);
        }
    }

    int ret = av_read_frame(session.fmtCtx.get(), session.pkt.get());
    if (ret < 0) {
        if (ret == AVERROR_EOF) {
            qCDebug(dragonsdlDecoder) << "EOF reached after" << session.packetCount << "packets," << session.frameCount << "frames";
            return false;
        }
        qCDebug(dragonsdlDecoder) << "av_read_frame transient error" << ret;
        av_packet_unref(session.pkt.get());
        return true;
    }
    ++session.packetCount;

    if (session.pkt->stream_index != session.audioStreamIndex) {
        av_packet_unref(session.pkt.get());
        return true;
    }

    ret = avcodec_send_packet(session.codecCtx.get(), session.pkt.get());
    av_packet_unref(session.pkt.get());

    if (ret < 0) {
        avcodec_flush_buffers(session.codecCtx.get());
        return true;
    }

    drainDecoderFrames(session, true);
    return true;
}

void DragonDecoder::drainDecoderFrames(DecodeSession &session, bool canEmitFirstFrame)
{
    while (true) {
        int ret = avcodec_receive_frame(session.codecCtx.get(), session.frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        auto frameGuard = qScopeGuard([&]() {
            av_frame_unref(session.frame.get());
        });

        int inSamples = session.frame->nb_samples;
        if (inSamples <= 0) {
            continue;
        }

        int delaySamples = swr_get_delay(session.swrCtx.get(), session.sampleRate);
        int maxOutSamples = delaySamples + inSamples;
        if (maxOutSamples <= 0) {
            continue;
        }

        size_t neededSize = static_cast<size_t>(maxOutSamples) * static_cast<size_t>(session.nbChannels);
        if (m_pcmBuffer.size() < neededSize) {
            m_pcmBuffer.resize(neededSize);
        }

        uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(m_pcmBuffer.data())};
        int converted = swr_convert(session.swrCtx.get(), outData, maxOutSamples, const_cast<const uint8_t **>(session.frame->data), session.frame->nb_samples);
        if (converted < 0) {
            qCWarning(dragonsdlDecoder) << "swr_convert failed";
            continue;
        }

        int totalSamples = converted * session.nbChannels;
        if (totalSamples > 0) {
            ++session.frameCount;
            if (canEmitFirstFrame && session.firstFrame) {
                session.firstFrame = false;
                qCDebug(dragonsdlDecoder) << "first frame" << totalSamples << "samples";
                Q_EMIT stateChanged(false, 1.0);
            }
            if (m_samplesCallback) {
                m_samplesCallback(std::span(m_pcmBuffer.data(), static_cast<size_t>(totalSamples)), session.sampleRate, session.nbChannels);
            }
        }
    }
}

void DragonDecoder::runMainDecodeLoop(DecodeSession &session, std::stop_token st)
{
    while (!st.stop_requested()) {
        if (!readAndProcessPacket(session)) {
            break;
        }
        if (st.stop_requested()) {
            break;
        }
    }
}

void DragonDecoder::flushDecoder(DecodeSession &session)
{
    qCDebug(dragonsdlDecoder) << "flushing decoder...";
    avcodec_send_packet(session.codecCtx.get(), nullptr);
    drainDecoderFrames(session, false);
}

void DragonDecoder::flushResampler(DecodeSession &session)
{
    int delaySamples = swr_get_delay(session.swrCtx.get(), session.sampleRate);
    if (delaySamples > 0) {
        size_t neededSize = static_cast<size_t>(delaySamples) * static_cast<size_t>(session.nbChannels);
        if (m_pcmBuffer.size() < neededSize) {
            m_pcmBuffer.resize(neededSize);
        }
        uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(m_pcmBuffer.data())};
        int converted = swr_convert(session.swrCtx.get(), outData, delaySamples, nullptr, 0);
        if (converted > 0) {
            int totalSamples = converted * session.nbChannels;
            qCDebug(dragonsdlDecoder) << "swr flush samplesDecoded" << totalSamples << "samples";
            if (m_samplesCallback) {
                m_samplesCallback(std::span(m_pcmBuffer.data(), static_cast<size_t>(totalSamples)), session.sampleRate, session.nbChannels);
            }
        }
    }
}

bool DragonDecoder::hasFatalError() const
{
    return m_hadFatalError.load(std::memory_order_relaxed);
}

void DragonDecoder::requestSeek(int64_t positionMs)
{
    m_seekTargetMs.store(std::max(int64_t{0}, positionMs), std::memory_order_relaxed);
    m_seekRequested.store(true, std::memory_order_release);
}

void DragonDecoder::setSamplesCallback(SamplesCallback cb)
{
    m_samplesCallback = std::move(cb);
}