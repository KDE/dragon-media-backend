/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragondecoder.h"
#include <stdfloat>

#include "dragonsdl_decoder_logging.h"
#include <QScopeGuard>

#include <array>
#include <chrono>
#include <cstring>
#include <generator>
#include <memory>
#include <ranges>
#include <span>
#include <thread>

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

struct AvioCtxDeleter {
    void operator()(AVIOContext *ctx) const noexcept
    {
        if (ctx) {
            uint8_t *buf = ctx->buffer;
            avio_context_free(&ctx);
            av_free(buf);
        }
    }
};

struct AvFormatCtxDeleter {
    void operator()(AVFormatContext *ctx) const noexcept
    {
        if (ctx) {
            avformat_close_input(&ctx);
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
    std::unique_ptr<AVIOContext, AvioCtxDeleter> avioCtx;

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

    int consecutiveReadErrors = 0;
    static constexpr int MAX_READ_ERRORS = 10;
    static constexpr auto READ_ERROR_RESET_INTERVAL = std::chrono::milliseconds(5000);
    std::chrono::steady_clock::time_point lastSuccessfulRead;
};

DragonDecoder::DragonDecoder(ReadCallback readCb, SeekCallback seekCb, const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_networkCallback(std::move(readCb))
    , m_seekCallback(std::move(seekCb))
    , m_filePath(filePath)
{
}

DragonDecoder::~DragonDecoder() = default;

DragonSdl::InitResult DragonDecoder::initialize()
{
    using namespace DragonSdl;

    if (m_session) {
        InitResult result;
        result.success = true;
        result.sampleRate = m_session->sampleRate;
        result.channels = m_session->nbChannels;
        result.durationMs = (m_session->fmtCtx->duration != AV_NOPTS_VALUE) ? m_session->fmtCtx->duration / (AV_TIME_BASE / 1000) : -1;
        return result;
    }

    av_log_set_level(AV_LOG_ERROR);

    m_session = std::make_unique<DecodeSession>();

    m_session->consecutiveReadErrors = 0;
    m_session->lastSuccessfulRead = std::chrono::steady_clock::now();

    InitResult result;
    result.success = false;

    if (!initializeAvio(*m_session)) {
        result.errorMessage = u"Failed to initialize AVIO"_s;
        return result;
    }
    if (!openContainer(*m_session)) {
        result.errorMessage = u"avformat_open_input failed"_s;
        return result;
    }
    if (!findAudioStream(*m_session)) {
        result.errorMessage = u"No supported audio stream found"_s;
        return result;
    }
    if (!setupCodec(*m_session)) {
        result.errorMessage = u"Codec setup failed"_s;
        return result;
    }
    if (!setupResampler(*m_session)) {
        result.errorMessage = u"Resampler setup failed"_s;
        return result;
    }
    if (!allocatePacketAndFrame(*m_session)) {
        result.errorMessage = u"Failed to allocate packet/frame"_s;
        return result;
    }

    result.durationMs = (m_session->fmtCtx->duration != AV_NOPTS_VALUE) ? m_session->fmtCtx->duration / (AV_TIME_BASE / 1000) : -1;
    result.sampleRate = m_session->sampleRate;
    result.channels = m_session->nbChannels;
    result.success = true;

    qCDebug(dragonsdlDecoder) << "initialize sr=" << result.sampleRate << "ch=" << result.channels;

    return result;
}

std::generator<DragonSdl::DecodeEvent> DragonDecoder::decodeLoop(std::stop_token st)
{
    using namespace DragonSdl;

    if (!m_session) {
        co_yield DecodeError{u"Decoder not initialized"_s};
        co_return;
    }

    DecodeSession &session = *m_session;

    while (!st.stop_requested()) {
        if (!readAndProcessPacket(session)) {
            break;
        }
        if (st.stop_requested()) {
            break;
        }

        while (auto chunk = drainDecoderFrames(session)) {
            if (session.firstFrame) {
                session.firstFrame = false;
                qCDebug(dragonsdlDecoder) << "first frame" << static_cast<int>(chunk->data.size()) << "samples";
            }
            co_yield std::move(*chunk);
        }
    }

    flushDecoder(session);
    while (auto chunk = drainDecoderFrames(session)) {
        co_yield std::move(*chunk);
    }

    if (auto chunk = flushResampler(session)) {
        co_yield std::move(*chunk);
    }

    co_yield DecodeEof{};

    qCDebug(dragonsdlDecoder) << "decodeLoop finished total packets=" << session.packetCount << "total frames=" << session.frameCount;
}

bool DragonDecoder::initializeAvio(DecodeSession &session)
{
    if (!m_networkCallback || !m_filePath.isEmpty()) {
        return true;
    }

    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(IO_BUFFER_SIZE));
    if (!ioBuffer) {
        qCWarning(dragonsdlDecoder) << "Failed to allocate AVIOContext buffer";
        return false;
    }

    auto readPacket = [](void *opaque, uint8_t *buf, int bufSize) -> int {
        auto *self = static_cast<DragonDecoder *>(opaque);
        const int ret = self->m_networkCallback(std::span(buf, static_cast<size_t>(bufSize)));
        return ret == 0 ? AVERROR_EOF : ret;
    };

    auto seekPacket = [](void *opaque, int64_t offset, int whence) -> int64_t {
        auto *self = static_cast<DragonDecoder *>(opaque);
        if (!self->m_seekCallback) {
            return AVERROR(ENOSYS);
        }

        SeekWhence sw;
        if (whence & AVSEEK_SIZE) {
            sw = SeekWhence::Size;
        } else {
            switch (whence & 0xF) {
            case 0:
                sw = SeekWhence::Set;
                break;
            case 1:
                sw = SeekWhence::Cur;
                break;
            case 2:
                sw = SeekWhence::End;
                break;
            default:
                return AVERROR(ENOSYS);
            }
        }

        const int64_t ret = self->m_seekCallback(offset, sw);
        return ret < 0 ? AVERROR(ENOSYS) : ret;
    };

    session.avioCtx.reset(avio_alloc_context(ioBuffer, IO_BUFFER_SIZE, 0, this, readPacket, nullptr, m_seekCallback ? seekPacket : nullptr));
    if (!session.avioCtx) {
        av_free(ioBuffer);
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }

    if (m_seekCallback) {
        const int64_t probeSize = m_seekCallback(0, SeekWhence::Size);
        if (probeSize > 0) {
            session.avioCtx->seekable = AVIO_SEEKABLE_NORMAL;
        }
    }

    return true;
}

bool DragonDecoder::openContainer(DecodeSession &session)
{
    AVFormatContext *rawFmtCtx = nullptr;

    if (qEnvironmentVariableIsSet("DRAGON_TEST_SLOW_OPEN")) {
        qCWarning(dragonsdlDecoder) << "DRAGON_TEST_SLOW_OPEN is set, sleeping 1000ms";
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    if (m_networkCallback) {
        rawFmtCtx = avformat_alloc_context();
        if (!rawFmtCtx) {
            return false;
        }
        rawFmtCtx->pb = session.avioCtx.get();
        rawFmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

        if (const int err = avformat_open_input(&rawFmtCtx, "", nullptr, nullptr); err < 0) {
            m_hadFatalError.store(true, std::memory_order_relaxed);
            return false;
        }
    } else {
        if (const int err = avformat_open_input(&rawFmtCtx, m_filePath.toUtf8().constData(), nullptr, nullptr); err < 0) {
            m_hadFatalError.store(true, std::memory_order_relaxed);
            return false;
        }
    }

    session.fmtCtx.reset(rawFmtCtx);
    return true;
}

bool DragonDecoder::findAudioStream(DecodeSession &session)
{
    if (const int err = avformat_find_stream_info(session.fmtCtx.get(), nullptr); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
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
        return false;
    }

    return true;
}

bool DragonDecoder::setupCodec(DecodeSession &session)
{
    CodecContextPtr codecCtx{avcodec_alloc_context3(session.codec)};
    if (!codecCtx) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }

    if (const int err = avcodec_parameters_to_context(codecCtx.get(), session.audioStream->codecpar); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }

    if (const int err = avcodec_open2(codecCtx.get(), session.codec, nullptr); err < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
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
        return false;
    }

    session.swrCtx.reset(rawSwrCtx);
    ret = swr_init(session.swrCtx.get());
    if (ret < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }

    return true;
}

bool DragonDecoder::allocatePacketAndFrame(DecodeSession &session)
{
    session.pkt.reset(av_packet_alloc());
    session.frame.reset(av_frame_alloc());
    if (!session.pkt || !session.frame) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool DragonDecoder::isRecoverableReadError(int errorCode) const
{
    switch (errorCode) {
    case AVERROR(EIO):
    case AVERROR(ECONNRESET):
    case AVERROR(EINVAL):
    case AVERROR_INVALIDDATA:
    case AVERROR_EXIT:
        return false;

    case AVERROR(EAGAIN):
    case AVERROR(ETIMEDOUT):
    case AVERROR(ECONNREFUSED):
        return true;

    default:
        return true;
    }
}

QString DragonDecoder::avErrorString(int errorCode) const
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> errbuf{};
    if (av_strerror(errorCode, errbuf.data(), errbuf.size()) < 0) {
        return u"Unknown FFmpeg error "_s % QString::number(errorCode);
    }
    return QString::fromUtf8(errbuf.data());
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
            if (swr_init(session.swrCtx.get()) < 0) {
                qCWarning(dragonsdlDecoder) << "swr_init failed after seek";
            }
        }
    }

    int readStatus = av_read_frame(session.fmtCtx.get(), session.pkt.get());
    if (readStatus < 0) {
        if (readStatus == AVERROR_EOF) {
            if (session.fmtCtx->pb && session.fmtCtx->pb->error < 0 && session.fmtCtx->pb->error != AVERROR(EAGAIN)) {
                readStatus = session.fmtCtx->pb->error;
            } else {
                qCDebug(dragonsdlDecoder) << "EOF reached after" << session.packetCount << "packets," << session.frameCount << "frames";
                return false;
            }
        }

        if (readStatus == AVERROR(EAGAIN)) {
            session.consecutiveReadErrors++;
            qCDebug(dragonsdlDecoder) << "av_read_frame EAGAIN, attempt " << session.consecutiveReadErrors;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            av_packet_unref(session.pkt.get());
            return true;
        }

        bool isRecoverable = isRecoverableReadError(readStatus);

        session.consecutiveReadErrors++;
        auto now = std::chrono::steady_clock::now();

        if (now - session.lastSuccessfulRead < session.READ_ERROR_RESET_INTERVAL) {
            if (session.consecutiveReadErrors <= 3) {
                qCDebug(dragonsdlDecoder) << "av_read_frame transient error" << readStatus << "(" << avErrorString(readStatus) << "), attempt "
                                          << session.consecutiveReadErrors;
                av_packet_unref(session.pkt.get());
                return true;
            }
        }

        if (session.consecutiveReadErrors >= session.MAX_READ_ERRORS || !isRecoverable) {
            QString errorMsg = u"Read error after "_s % QString::number(session.consecutiveReadErrors) % u" attempts: "_s % avErrorString(readStatus);

            qCWarning(dragonsdlDecoder) << errorMsg;

            Q_EMIT streamError(errorMsg);

            m_hadFatalError.store(true, std::memory_order_relaxed);
            av_packet_unref(session.pkt.get());
            return false;
        }

        qCWarning(dragonsdlDecoder) << "av_read_frame error" << readStatus << "(" << avErrorString(readStatus) << "), attempt " << session.consecutiveReadErrors
                                    << "/" << session.MAX_READ_ERRORS;
        av_packet_unref(session.pkt.get());
        return true;
    }

    session.consecutiveReadErrors = 0;
    session.lastSuccessfulRead = std::chrono::steady_clock::now();

    ++session.packetCount;

    if (session.pkt->stream_index != session.audioStreamIndex) {
        av_packet_unref(session.pkt.get());
        return true;
    }

    int sendStatus = avcodec_send_packet(session.codecCtx.get(), session.pkt.get());
    av_packet_unref(session.pkt.get());

    if (sendStatus < 0) {
        avcodec_flush_buffers(session.codecCtx.get());
        return true;
    }

    return true;
}

std::optional<DragonSdl::SamplesChunk> DragonDecoder::drainDecoderFrames(DecodeSession &session)
{
    using namespace DragonSdl;

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


            m_pendingSamples.assign(m_pcmBuffer.begin(), m_pcmBuffer.begin() + totalSamples);

            return SamplesChunk{.data = std::span<const std::float32_t>(m_pendingSamples.data(), m_pendingSamples.size()),
                                .sampleRate = session.sampleRate,
                                .channels = session.nbChannels};
        }
    }

    return std::nullopt;
}

void DragonDecoder::flushDecoder(DecodeSession &session)
{
    qCDebug(dragonsdlDecoder) << "flushing decoder...";
    avcodec_send_packet(session.codecCtx.get(), nullptr);
}

std::optional<DragonSdl::SamplesChunk> DragonDecoder::flushResampler(DecodeSession &session)
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

            m_pendingSamples.assign(m_pcmBuffer.begin(), m_pcmBuffer.begin() + totalSamples);

            return DragonSdl::SamplesChunk{.data = std::span<const std::float32_t>(m_pendingSamples.data(), m_pendingSamples.size()),
                                           .sampleRate = session.sampleRate,
                                           .channels = session.nbChannels};
        }
    }
    return std::nullopt;
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
