/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragondecoder.h"

#include "dragonmediabackend_decoder_logging.h"
#include <KLocalizedString>
#include <QScopeGuard>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <span>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

using namespace Qt::StringLiterals;

static constexpr int IO_BUFFER_SIZE = 65536;

// FFmpeg container durations are in AV_TIME_BASE units, which are microseconds.
static std::optional<std::chrono::milliseconds> containerDuration(int64_t avDurationUs)
{
    if (avDurationUs == AV_NOPTS_VALUE) {
        return std::nullopt;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::microseconds{avDurationUs});
}

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

    // Free-list of reusable sample buffers.
    struct SampleBufferPool : std::enable_shared_from_this<SampleBufferPool> {
        std::vector<std::unique_ptr<std::vector<float>>> freeList;
        static constexpr size_t kMaxFreeBuffers = 8;

        std::shared_ptr<std::vector<float>> acquire(size_t minSize)
        {
            for (auto it = freeList.begin(); it != freeList.end(); ++it) {
                if ((*it)->capacity() >= minSize) {
                    auto raw = std::move(*it);
                    freeList.erase(it);
                    return wrap(std::move(raw));
                }
            }
            return wrap(std::make_unique<std::vector<float>>(minSize));
        }

    private:
        std::shared_ptr<std::vector<float>> wrap(std::unique_ptr<std::vector<float>> raw)
        {
            // The deleter captures a shared_ptr to this pool, extending the
            // pool's lifetime until the last outstanding buffer is returned.
            auto poolRef = shared_from_this();
            auto *rawPtr = raw.release();
            return {rawPtr, [poolRef = std::move(poolRef)](std::vector<float> *p) {
                        poolRef->recycle(p);
                    }};
        }

        void recycle(std::vector<float> *p)
        {
            std::unique_ptr<std::vector<float>> owned(p);
            if (freeList.size() < kMaxFreeBuffers) {
                freeList.push_back(std::move(owned));
            }
            // else: owned destructs here and the buffer is freed.
        }
    };

    std::shared_ptr<SampleBufferPool> m_bufferPool = std::make_shared<SampleBufferPool>();
};

DragonDecoder::DragonDecoder(ReadCallback readCb, SeekCallback seekCb, qint64 streamSize, const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_networkCallback(std::move(readCb))
    , m_seekCallback(std::move(seekCb))
    , m_streamSize(streamSize)
    , m_filePath(filePath)
{
}

DragonDecoder::~DragonDecoder() = default;

DragonMediaBackend::InitResult DragonDecoder::initialize()
{
    using namespace DragonMediaBackend;

    if (m_session) {
        InitResult result;
        result.success = true;
        result.sampleRate = m_session->sampleRate;
        result.channels = m_session->nbChannels;
        result.duration = containerDuration(m_session->fmtCtx->duration);
        return result;
    }

    av_log_set_level(AV_LOG_ERROR);

    m_session = std::make_unique<DecodeSession>();

    m_session->consecutiveReadErrors = 0;
    m_session->lastSuccessfulRead = std::chrono::steady_clock::now();

    InitResult result;
    result.success = false;

    if (!initializeAvio(*m_session)) {
        result.errorMessage = i18n("Failed to initialize media input");
        return result;
    }
    if (!openContainer(*m_session)) {
        result.errorMessage = i18n("Failed to open media container");
        return result;
    }
    if (!findAudioStream(*m_session)) {
        result.errorMessage = i18n("No supported audio stream found");
        return result;
    }
    if (!setupCodec(*m_session)) {
        result.errorMessage = i18n("Audio codec setup failed");
        return result;
    }
    if (!setupResampler(*m_session)) {
        result.errorMessage = i18n("Audio resampler setup failed");
        return result;
    }
    if (!allocatePacketAndFrame(*m_session)) {
        result.errorMessage = i18n("Failed to allocate decoder resources");
        return result;
    }

    result.duration = containerDuration(m_session->fmtCtx->duration);
    result.sampleRate = m_session->sampleRate;
    result.channels = m_session->nbChannels;
    result.success = true;

    qCDebug(dragonMediaBackendDecoder) << "initialize sr=" << result.sampleRate << "ch=" << result.channels;

    return result;
}

dragon::compat::generator<DragonMediaBackend::DecodeEvent> DragonDecoder::decodeLoop(std::stop_token st)
{
    using namespace DragonMediaBackend;

    if (!m_session) {
        co_yield DecodeError{i18n("Decoder not initialized")};
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
                qCDebug(dragonMediaBackendDecoder) << "first frame" << static_cast<int>(chunk->data.size()) << "samples";
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

    qCDebug(dragonMediaBackendDecoder) << "decodeLoop finished total packets=" << session.packetCount << "total frames=" << session.frameCount;
}

bool DragonDecoder::initializeAvio(DecodeSession &session)
{
    if (!m_networkCallback || !m_filePath.isEmpty()) {
        return true;
    }

    uint8_t *ioBuffer = static_cast<uint8_t *>(av_malloc(IO_BUFFER_SIZE));
    if (!ioBuffer) {
        qCWarning(dragonMediaBackendDecoder) << "Failed to allocate AVIOContext buffer";
        return false;
    }

    auto readPacket = [](void *opaque, uint8_t *buf, int bufSize) -> int {
        auto *self = static_cast<DragonDecoder *>(opaque);
        const int ret = self->m_networkCallback(std::span(buf, static_cast<size_t>(bufSize)));
        if (ret == 0)
            return AVERROR_EOF;
        return ret < 0 ? AVERROR(EIO) : ret;
    };

    auto seekPacket = [](void *opaque, int64_t offset, int whence) -> int64_t {
        auto *self = static_cast<DragonDecoder *>(opaque);
        if (!self->m_seekCallback) {
            return AVERROR(ENOSYS);
        }

        SeekWhence sw;
        if (whence & AVSEEK_SIZE) {
            return self->m_streamSize > 0 ? self->m_streamSize : AVERROR(ENOSYS);
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

        const qint64 ret = self->m_seekCallback(offset, sw);
        return ret < 0 ? AVERROR(ENOSYS) : ret;
    };

    // seekFn only needed to workaround a GCC 13 bug https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94039
    int64_t (*seekFn)(void *opaque, int64_t offset, int whence) = seekPacket;
    session.avioCtx.reset(avio_alloc_context(ioBuffer, IO_BUFFER_SIZE, 0, this, readPacket, nullptr, m_seekCallback ? seekFn : nullptr));
    if (!session.avioCtx) {
        av_free(ioBuffer);
        m_hadFatalError.store(true, std::memory_order_relaxed);
        return false;
    }

    if (m_seekCallback) {
        if (m_streamSize > 0) {
            session.avioCtx->seekable = AVIO_SEEKABLE_NORMAL;
        }
    }

    return true;
}

bool DragonDecoder::openContainer(DecodeSession &session)
{
    AVFormatContext *rawFmtCtx = nullptr;

    if (qEnvironmentVariableIsSet("DRAGON_TEST_SLOW_OPEN")) {
        qCWarning(dragonMediaBackendDecoder) << "DRAGON_TEST_SLOW_OPEN is set, sleeping 1000ms";
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

    const std::span streams{session.fmtCtx->streams, session.fmtCtx->nb_streams};
    const auto it = std::ranges::find_if(streams, [&session](const AVStream *stream) {
        const AVCodecParameters *par = stream->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_AUDIO) {
            return false;
        }
        const AVCodec *decoder = avcodec_find_decoder(par->codec_id);
        if (!decoder) {
            return false;
        }
        session.codec = decoder;
        return true;
    });

    if (it != streams.end()) {
        session.audioStream = *it;
        session.audioStreamIndex = static_cast<int>(std::ranges::distance(streams.begin(), it));
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
    case AVERROR(EINTR):
    case AVERROR(ETIMEDOUT):
    case AVERROR(ECONNREFUSED):
        return true;

    default:
        return false;
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
        const auto targetMs = std::chrono::milliseconds{m_seekTarget.load(std::memory_order_relaxed)};
        AVRational msTimeBase = AVRational{1, 1000};
        qint64 streamTimestamp = av_rescale_q(targetMs.count(), msTimeBase, session.audioStream->time_base);
        int seekRet = av_seek_frame(session.fmtCtx.get(), session.audioStreamIndex, streamTimestamp, AVSEEK_FLAG_BACKWARD);
        if (seekRet < 0) {
            qCWarning(dragonMediaBackendDecoder) << "seek to" << targetMs << "failed:" << seekRet << "(" << avErrorString(seekRet)
                                                 << "), continuing from current position";
        } else {
            avcodec_flush_buffers(session.codecCtx.get());
            if (const int swrRet = swr_init(session.swrCtx.get()); swrRet < 0) {
                // A failed resampler reinit leaves swr in a bad state and
                // every subsequent swr_convert would fail; treat as fatal.
                qCWarning(dragonMediaBackendDecoder) << "swr_init failed after seek:" << swrRet << "(" << avErrorString(swrRet) << ")";
                Q_EMIT streamError(i18n("Resampler reinitialization failed after seek"));
                m_hadFatalError.store(true, std::memory_order_relaxed);
                return false;
            }
        }
    }

    int readStatus = av_read_frame(session.fmtCtx.get(), session.pkt.get());
    if (readStatus < 0) {
        if (readStatus == AVERROR_EOF) {
            if (session.fmtCtx->pb && session.fmtCtx->pb->error < 0 && session.fmtCtx->pb->error != AVERROR(EAGAIN)) {
                readStatus = session.fmtCtx->pb->error;
            } else {
                qCDebug(dragonMediaBackendDecoder) << "EOF reached after" << session.packetCount << "packets," << session.frameCount << "frames";
                return false;
            }
        }

        if (readStatus == AVERROR(EAGAIN)) {
            session.consecutiveReadErrors++;
            qCDebug(dragonMediaBackendDecoder) << "av_read_frame EAGAIN, attempt " << session.consecutiveReadErrors;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            av_packet_unref(session.pkt.get());
            return true;
        }

        bool isRecoverable = isRecoverableReadError(readStatus);

        session.consecutiveReadErrors++;
        auto now = std::chrono::steady_clock::now();

        if (now - session.lastSuccessfulRead < session.READ_ERROR_RESET_INTERVAL) {
            if (session.consecutiveReadErrors <= 3) {
                qCDebug(dragonMediaBackendDecoder) << "av_read_frame transient error" << readStatus << "(" << avErrorString(readStatus) << "), attempt "
                                                   << session.consecutiveReadErrors;
                av_packet_unref(session.pkt.get());
                return true;
            }
        }

        if (session.consecutiveReadErrors >= session.MAX_READ_ERRORS || !isRecoverable) {
            QString errorMsg = i18n("Read error after %1 attempts: %2", session.consecutiveReadErrors, avErrorString(readStatus));

            qCWarning(dragonMediaBackendDecoder) << errorMsg;

            Q_EMIT streamError(errorMsg);

            m_hadFatalError.store(true, std::memory_order_relaxed);
            av_packet_unref(session.pkt.get());
            return false;
        }

        qCWarning(dragonMediaBackendDecoder) << "av_read_frame error" << readStatus << "(" << avErrorString(readStatus) << "), attempt "
                                             << session.consecutiveReadErrors << "/" << session.MAX_READ_ERRORS;
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

std::optional<DragonMediaBackend::SamplesChunk> DragonDecoder::drainDecoderFrames(DecodeSession &session)
{
    using namespace DragonMediaBackend;

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

        const int delaySamples = swr_get_delay(session.swrCtx.get(), session.sampleRate);
        const int maxOutSamples = delaySamples + inSamples;
        if (maxOutSamples <= 0) {
            continue;
        }

        if (auto chunk = resampleInto(session, const_cast<const uint8_t *const *>(session.frame->data), inSamples, maxOutSamples)) {
            ++session.frameCount;
            return chunk;
        }
    }

    return std::nullopt;
}

void DragonDecoder::flushDecoder(DecodeSession &session)
{
    qCDebug(dragonMediaBackendDecoder) << "flushing decoder...";
    avcodec_send_packet(session.codecCtx.get(), nullptr);
}

std::optional<DragonMediaBackend::SamplesChunk> DragonDecoder::flushResampler(DecodeSession &session)
{
    const int delaySamples = swr_get_delay(session.swrCtx.get(), session.sampleRate);
    if (delaySamples <= 0) {
        return std::nullopt;
    }
    auto chunk = resampleInto(session, nullptr, 0, delaySamples);
    if (chunk) {
        qCDebug(dragonMediaBackendDecoder) << "swr flush samplesDecoded" << chunk->data.size() << "samples";
    }
    return chunk;
}

std::optional<DragonMediaBackend::SamplesChunk> DragonDecoder::resampleInto(DecodeSession &session, const uint8_t *const *in, int inSamples, int maxOutSamples)
{
    using namespace DragonMediaBackend;

    const size_t neededSize = static_cast<size_t>(maxOutSamples) * static_cast<size_t>(session.nbChannels);
    auto ownedBuffer = session.m_bufferPool->acquire(neededSize);

    uint8_t *outData[1] = {reinterpret_cast<uint8_t *>(ownedBuffer->data())};
#if LIBSWRESAMPLE_VERSION_INT < AV_VERSION_INT(4, 14, 100)
    const int converted = swr_convert(session.swrCtx.get(), outData, maxOutSamples, const_cast<const uint8_t **>(in), inSamples);
#else
    const int converted = swr_convert(session.swrCtx.get(), outData, maxOutSamples, in, inSamples);
#endif
    if (converted < 0) {
        qCWarning(dragonMediaBackendDecoder) << "swr_convert failed";
        return std::nullopt;
    }

    const int totalSamples = converted * session.nbChannels;
    if (totalSamples <= 0) {
        return std::nullopt;
    }

    return SamplesChunk::owning(std::move(ownedBuffer), session.sampleRate, session.nbChannels, static_cast<size_t>(totalSamples));
}

bool DragonDecoder::hasFatalError() const
{
    return m_hadFatalError.load(std::memory_order_relaxed);
}

void DragonDecoder::requestSeek(std::chrono::milliseconds position)
{
    m_seekTarget.store(std::max(std::chrono::milliseconds::rep{0}, position.count()), std::memory_order_relaxed);
    m_seekRequested.store(true, std::memory_order_release);
}
