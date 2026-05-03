/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragondecoder.h>
#include <stdfloat>

#include <QDebug>
#include <QScopeGuard>

#include <cstring>
#include <memory>
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

struct SwrCtxDeleter {
    void operator()(SwrContext *ctx) const noexcept
    {
        if (ctx) {
            swr_free(&ctx);
        }
    }
};

DragonDecoder::DragonDecoder(ReadCallback readCb, const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_readCb(std::move(readCb))
    , m_filePath(filePath)
{
}

DragonDecoder::~DragonDecoder() = default;

void DragonDecoder::decodeLoop(std::stop_token st)
{
    av_log_set_level(AV_LOG_ERROR);

    AVIOContext *avioCtx = nullptr;
    uint8_t *ioBuffer = nullptr;

    auto readPacket = [](void *opaque, uint8_t *buf, int bufSize) -> int {
        auto *self = static_cast<DragonDecoder *>(opaque);
        int ret = self->m_readCb(std::span(buf, static_cast<size_t>(bufSize)));
        return ret == 0 ? AVERROR_EOF : ret;
    };

    if (m_readCb && m_filePath.isEmpty()) {
        ioBuffer = static_cast<uint8_t *>(av_malloc(IO_BUFFER_SIZE));
        if (!ioBuffer) {
            emit streamError(u"Failed to allocate AVIOContext buffer"_s);
            return;
        }

        avioCtx = avio_alloc_context(ioBuffer, IO_BUFFER_SIZE, 0, this, readPacket, nullptr, nullptr);
        if (!avioCtx) {
            av_free(ioBuffer);
            m_hadFatalError.store(true, std::memory_order_relaxed);
            emit streamError(u"Failed to create AVIOContext"_s);
            return;
        }
    }

    AVFormatContext *rawFmtCtx = nullptr;

    if (m_filePath.isEmpty()) {
        rawFmtCtx = avformat_alloc_context();
        if (!rawFmtCtx) {
            uint8_t *currentBuffer = avioCtx ? avioCtx->buffer : nullptr;
            avio_context_free(&avioCtx);
            av_free(currentBuffer);
            m_hadFatalError.store(true, std::memory_order_relaxed);
            emit streamError(u"Failed to allocate AVFormatContext"_s);
            return;
        }
        rawFmtCtx->pb = avioCtx;

        int ret = avformat_open_input(&rawFmtCtx, nullptr, nullptr, nullptr);
        if (ret < 0) {
            if (rawFmtCtx) {
                rawFmtCtx->pb = nullptr;
                avformat_free_context(rawFmtCtx);
            }
            uint8_t *currentBuffer = avioCtx ? avioCtx->buffer : nullptr;
            avio_context_free(&avioCtx);
            av_free(currentBuffer);
            m_hadFatalError.store(true, std::memory_order_relaxed);
            emit streamError(u"avformat_open_input failed"_s);
            return;
        }
    } else {
        int ret = avformat_open_input(&rawFmtCtx, m_filePath.toUtf8().constData(), nullptr, nullptr);
        if (ret < 0) {
            m_hadFatalError.store(true, std::memory_order_relaxed);
            emit streamError(u"avformat_open_input failed for %1"_s.arg(m_filePath));
            return;
        }
    }

    std::unique_ptr<AVFormatContext, AvFormatCtxDeleter> fmtCtx(rawFmtCtx);

    int ret = avformat_find_stream_info(fmtCtx.get(), nullptr);
    if (ret < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"avformat_find_stream_info failed"_s);
        return;
    }

    int audioStreamIndex = -1;
    const AVCodec *codec = nullptr;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream *stream = fmtCtx->streams[i];
        const AVCodecParameters *par = stream->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            codec = avcodec_find_decoder(par->codec_id);
            if (codec) {
                audioStreamIndex = static_cast<int>(i);
                break;
            }
        }
    }

    if (audioStreamIndex < 0 || !codec) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"No supported audio stream found"_s);
        return;
    }

    AVStream *audioStream = fmtCtx->streams[audioStreamIndex];
    const AVCodecParameters *codecPar = audioStream->codecpar;

    AVCodecContext *rawCodecCtx = avcodec_alloc_context3(codec);
    if (!rawCodecCtx) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"Failed to allocate codec context"_s);
        return;
    }
    ret = avcodec_parameters_to_context(rawCodecCtx, codecPar);
    if (ret < 0) {
        avcodec_free_context(&rawCodecCtx);
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"avcodec_parameters_to_context failed"_s);
        return;
    }
    ret = avcodec_open2(rawCodecCtx, codec, nullptr);
    if (ret < 0) {
        avcodec_free_context(&rawCodecCtx);
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"avcodec_open2 failed"_s);
        return;
    }
    std::unique_ptr<AVCodecContext, AvCodecCtxDeleter> codecCtx(rawCodecCtx);

    int sampleRate = codecCtx->sample_rate;
    int nbChannels = codecCtx->ch_layout.nb_channels;

    SwrContext *rawSwrCtx = nullptr;
    AVChannelLayout outLayout = codecCtx->ch_layout;
    ret = swr_alloc_set_opts2(&rawSwrCtx, &outLayout, AV_SAMPLE_FMT_FLT, sampleRate, &codecCtx->ch_layout, codecCtx->sample_fmt, sampleRate, 0, nullptr);
    if (ret < 0 || !rawSwrCtx) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"swr_alloc_set_opts2 failed"_s);
        return;
    }
    std::unique_ptr<SwrContext, SwrCtxDeleter> swrCtx(rawSwrCtx);
    ret = swr_init(swrCtx.get());
    if (ret < 0) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"swr_init failed"_s);
        return;
    }

    qDebug() << "DECODER: formatReady sr=" << sampleRate << "ch=" << nbChannels << "codec=" << codec->name;
    emit formatReady(sampleRate, nbChannels);

    if (fmtCtx->duration != AV_NOPTS_VALUE) {
        const int64_t durationMs = fmtCtx->duration / (AV_TIME_BASE / 1000);
        qDebug() << "DECODER: duration=" << durationMs << "ms";
        emit durationChanged(durationMs);
    } else {
        qDebug() << "DECODER: duration unknown";
    }

    std::unique_ptr<AVPacket, AvPacketDeleter> pkt(av_packet_alloc());
    std::unique_ptr<AVFrame, AvFrameDeleter> frame(av_frame_alloc());
    if (!pkt || !frame) {
        m_hadFatalError.store(true, std::memory_order_relaxed);
        emit streamError(u"Failed to allocate packet/frame"_s);
        return;
    }

    bool firstFrame = true;
    int packetCount = 0;
    int frameCount = 0;
    while (!st.stop_requested()) {
        if (m_seekRequested.exchange(false, std::memory_order_acq_rel)) {
            int64_t targetMs = m_seekTargetMs.load(std::memory_order_relaxed);
            AVRational msTimeBase = AVRational{1, 1000};
            int64_t streamTimestamp = av_rescale_q(targetMs, msTimeBase, audioStream->time_base);
            int seekRet = av_seek_frame(fmtCtx.get(), audioStreamIndex, streamTimestamp, AVSEEK_FLAG_BACKWARD);
            if (seekRet >= 0) {
                avcodec_flush_buffers(codecCtx.get());
                swr_convert(swrCtx.get(), nullptr, 0, nullptr, 0);
            }
        }

        ret = av_read_frame(fmtCtx.get(), pkt.get());
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                qDebug() << "DECODER: EOF reached after" << packetCount << "packets," << frameCount << "frames";
                break;
            }
            qDebug() << "DECODER: av_read_frame transient error" << ret;
            av_packet_unref(pkt.get());
            continue;
        }
        ++packetCount;

        if (pkt->stream_index != audioStreamIndex) {
            av_packet_unref(pkt.get());
            continue;
        }

        ret = avcodec_send_packet(codecCtx.get(), pkt.get());
        av_packet_unref(pkt.get());

        if (ret < 0) {
            avcodec_flush_buffers(codecCtx.get());
            continue;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(codecCtx.get(), frame.get());
            if (ret == AVERROR(EAGAIN)) {
                break;
            }
            if (ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                continue;
            }

            auto frameGuard = qScopeGuard([&]() {
                av_frame_unref(frame.get());
            });

            int inSamples = frame->nb_samples;
            if (inSamples <= 0) {
                continue;
            }

            int delaySamples = swr_get_delay(swrCtx.get(), sampleRate);
            int maxOutSamples = delaySamples + inSamples;
            if (maxOutSamples <= 0) {
                continue;
            }

            size_t neededSize = static_cast<size_t>(maxOutSamples) * static_cast<size_t>(nbChannels);
            if (m_pcmBuffer.size() < neededSize) {
                m_pcmBuffer.resize(neededSize);
            }

            uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(m_pcmBuffer.data())};
            int converted = swr_convert(swrCtx.get(), outData, maxOutSamples, const_cast<const uint8_t **>(frame->data), frame->nb_samples);
            if (converted < 0) {
                qWarning() << "swr_convert failed";
                continue;
            }

            int totalSamples = converted * nbChannels;
            if (totalSamples > 0) {
                ++frameCount;
                if (firstFrame) {
                    firstFrame = false;
                    qDebug() << "DECODER: first frame" << totalSamples << "samples";
                    emit stateChanged(false, 1.0);
                }
                if (m_samplesCallback) {
                    m_samplesCallback(std::span(m_pcmBuffer.data(), static_cast<size_t>(totalSamples)), sampleRate, nbChannels);
                }
            }
        }

        if (st.stop_requested()) {
            break;
        }
    }

    qDebug() << "DECODER: flushing decoder...";

    avcodec_send_packet(codecCtx.get(), nullptr);
    while (true) {
        ret = avcodec_receive_frame(codecCtx.get(), frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            continue;
        }

        auto frameGuard = qScopeGuard([&]() {
            av_frame_unref(frame.get());
        });

        int inSamples = frame->nb_samples;
        if (inSamples <= 0) {
            continue;
        }

        int delaySamples = swr_get_delay(swrCtx.get(), sampleRate);
        int maxOutSamples = delaySamples + inSamples;
        if (maxOutSamples <= 0) {
            continue;
        }

        size_t neededSize = static_cast<size_t>(maxOutSamples) * static_cast<size_t>(nbChannels);
        if (m_pcmBuffer.size() < neededSize) {
            m_pcmBuffer.resize(neededSize);
        }

        uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(m_pcmBuffer.data())};
        int converted = swr_convert(swrCtx.get(), outData, maxOutSamples, const_cast<const uint8_t **>(frame->data), frame->nb_samples);
        if (converted > 0) {
            int totalSamples = converted * nbChannels;
            qDebug() << "DECODER: flush samplesDecoded" << totalSamples << "samples";
            if (m_samplesCallback) {
                m_samplesCallback(std::span(m_pcmBuffer.data(), static_cast<size_t>(totalSamples)), sampleRate, nbChannels);
            }
        }
    }

    {
        int delaySamples = swr_get_delay(swrCtx.get(), sampleRate);
        if (delaySamples > 0) {
            size_t neededSize = static_cast<size_t>(delaySamples) * static_cast<size_t>(nbChannels);
            if (m_pcmBuffer.size() < neededSize) {
                m_pcmBuffer.resize(neededSize);
            }
            uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(m_pcmBuffer.data())};
            int converted = swr_convert(swrCtx.get(), outData, delaySamples, nullptr, 0);
            if (converted > 0) {
                int totalSamples = converted * nbChannels;
                qDebug() << "DECODER: swr flush samplesDecoded" << totalSamples << "samples";
                if (m_samplesCallback) {
                    m_samplesCallback(std::span(m_pcmBuffer.data(), static_cast<size_t>(totalSamples)), sampleRate, nbChannels);
                }
            }
        }
    }

    qDebug() << "DECODER: decodeLoop finished total packets=" << packetCount << "total frames=" << frameCount;
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