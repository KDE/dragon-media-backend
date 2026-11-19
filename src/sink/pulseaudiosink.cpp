/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "pulseaudiosink.h"

#include "dragonmediabackend_audio_logging.h"
#include "pulsesilencefill.h"

#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(PulseAudioSink, "pulseaudio_sink.json")

#include <QGuiApplication>
#include <QScopeGuard>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>
#include <utility>

using namespace Qt::StringLiterals;

static constexpr int kDefaultBufferMs = 100;
static constexpr int kDefaultTlengthMultiplier = 2;

namespace
{

struct MainloopDeleter {
    void operator()(pa_threaded_mainloop *ml) const noexcept
    {
        if (ml) {
            pa_threaded_mainloop_stop(ml);
            pa_threaded_mainloop_free(ml);
        }
    }
};

struct ContextDeleter {
    void operator()(pa_context *ctx) const noexcept
    {
        if (ctx) {
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
        }
    }
};

struct StreamDeleter {
    void operator()(pa_stream *s) const noexcept
    {
        if (s) {
            pa_stream_disconnect(s);
            pa_stream_unref(s);
        }
    }
};

using MainloopPtr = std::unique_ptr<pa_threaded_mainloop, MainloopDeleter>;
using ContextPtr = std::unique_ptr<pa_context, ContextDeleter>;
using PaStreamPtr = std::unique_ptr<pa_stream, StreamDeleter>;

class ScopedMainloopLock
{
public:
    explicit ScopedMainloopLock(pa_threaded_mainloop *ml)
        : m_mainloop(ml)
    {
        pa_threaded_mainloop_lock(m_mainloop);
    }

    ~ScopedMainloopLock()
    {
        pa_threaded_mainloop_unlock(m_mainloop);
    }

    ScopedMainloopLock(const ScopedMainloopLock &) = delete;
    ScopedMainloopLock &operator=(const ScopedMainloopLock &) = delete;

private:
    pa_threaded_mainloop *m_mainloop;
};

} // namespace

struct PulseAudioSinkStream::PaStream {
    PaStreamPtr handle;
};

struct PulseAudioSink::PaState {
    MainloopPtr mainloop;
    ContextPtr context;
    bool contextReady = false;

    // Sink-input index to owning stream; guarded by the mainloop lock.
    // Entries are removed before the stream is destroyed.
    std::unordered_map<uint32_t, PulseAudioSinkStream *> streamsByIndex;
};

PulseAudioSink::PulseAudioSink(QObject *parent, const QVariantList &args)
    : AudioSink(parent)
    , m_pa(std::make_unique<PaState>())
{
    Q_UNUSED(args);
    qCDebug(dragonMediaBackendAudio) << "PulseAudio audio device created";
}

PulseAudioSink::~PulseAudioSink()
{
    if (m_poisoned) {
        std::ignore = m_pa.release();
        return;
    }
    disconnectFromServer();
}

bool PulseAudioSink::probe()
{
    qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() checking daemon connectivity";

    MainloopPtr ml(pa_threaded_mainloop_new());
    if (!ml) {
        qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() pa_threaded_mainloop_new failed";
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(ml.get());
    ContextPtr ctx(pa_context_new(api, "dragon-probe"));
    if (!ctx) {
        qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() pa_context_new failed";
        return false;
    }

    if (pa_threaded_mainloop_start(ml.get()) < 0) {
        qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() pa_threaded_mainloop_start failed";
        return false;
    }

    static auto probeContextStateCallback = [](pa_context *c, void *userdata) {
        Q_UNUSED(c);
        auto *ml = static_cast<pa_threaded_mainloop *>(userdata);
        pa_threaded_mainloop_signal(ml, 0);
    };

    bool alive = false;
    {
        ScopedMainloopLock lock(ml.get());
        pa_context_set_state_callback(ctx.get(), probeContextStateCallback, ml.get());

        if (pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() pa_context_connect failed";
            pa_context_set_state_callback(ctx.get(), nullptr, nullptr);
            return false;
        }

        pa_context_state_t state = pa_context_get_state(ctx.get());
        auto startTime = std::chrono::steady_clock::now();
        constexpr auto kProbeTimeout = std::chrono::milliseconds(500);
        while (PA_CONTEXT_IS_GOOD(state) && state != PA_CONTEXT_READY) {
            pa_threaded_mainloop_wait(ml.get());
            state = pa_context_get_state(ctx.get());
            auto elapsed = std::chrono::steady_clock::now() - startTime;
            if (elapsed > kProbeTimeout) {
                qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() timed out waiting for context";
                break;
            }
        }

        alive = PA_CONTEXT_IS_GOOD(state) && (state == PA_CONTEXT_READY);
        if (alive) {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() daemon reachable";
        } else {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() daemon unreachable";
        }

        pa_context_set_state_callback(ctx.get(), nullptr, nullptr);
        pa_context_disconnect(ctx.get());
    }

    pa_threaded_mainloop_stop(ml.get());
    return alive;
}

PulseAudioSink::ConnectResult PulseAudioSink::connectToServer()
{
    MainloopPtr ml(pa_threaded_mainloop_new());
    if (!ml) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_threaded_mainloop_new failed";
        return std::unexpected(ConnectError{i18n("PulseAudio: failed to create mainloop")});
    }

    pa_threaded_mainloop_set_name(ml.get(), "dragon-pa");

    if (pa_threaded_mainloop_start(ml.get()) < 0) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_threaded_mainloop_start failed";
        return std::unexpected(ConnectError{i18n("PulseAudio: failed to start mainloop")});
    }

    // From this point onward the PA thread may signal, so publish the mainloop
    // pointer that the state callback uses.
    m_pa->mainloop = std::move(ml);

    auto cleanupGuard = qScopeGuard([this]() {
        disconnectFromServer();
    });

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_pa->mainloop.get());
    if (!api) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_threaded_mainloop_get_api returned null";
        return std::unexpected(ConnectError{i18n("PulseAudio: failed to get mainloop API")});
    }

    pa_proplist *rawProplist = pa_proplist_new();
    auto proplistCleanup = qScopeGuard([rawProplist]() {
        if (rawProplist) {
            pa_proplist_free(rawProplist);
        }
    });
    if (rawProplist) {
        const QString appName = QGuiApplication::applicationDisplayName();
        pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_NAME, appName.isEmpty() ? "DragonMediaBackend" : appName.toUtf8().constData());
        pa_proplist_sets(rawProplist, PA_PROP_MEDIA_ROLE, "music");

        const QString appId = AudioSink::applicationIconName();
        if (!appId.isEmpty()) {
            const QByteArray appIdUtf8 = appId.toUtf8();
            pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_ICON_NAME, appIdUtf8.constData());
            pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_ID, appIdUtf8.constData());
        }
    }

    ContextPtr ctx(pa_context_new_with_proplist(api, defaultStreamName().toUtf8().constData(), rawProplist));
    if (!ctx) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_context_new_with_proplist failed";
        return std::unexpected(ConnectError{i18n("PulseAudio: failed to create context")});
    }

    {
        ScopedMainloopLock lock(m_pa->mainloop.get());

        pa_context_set_state_callback(ctx.get(), PulseAudioSink::contextStateCallback, this);

        if (pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_context_connect failed";
            return std::unexpected(ConnectError{i18n("PulseAudio: failed to connect context")});
        }

        pa_context_state_t state = pa_context_get_state(ctx.get());
        while (PA_CONTEXT_IS_GOOD(state) && state != PA_CONTEXT_READY) {
            pa_threaded_mainloop_wait(m_pa->mainloop.get());
            state = pa_context_get_state(ctx.get());
        }

        if (!PA_CONTEXT_IS_GOOD(state) || state != PA_CONTEXT_READY) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: context did not become ready, state:" << state
                                                << "error:" << pa_strerror(pa_context_errno(ctx.get()));
            return std::unexpected(ConnectError{i18n("PulseAudio: context not ready"), true});
        }

        pa_context_set_subscribe_callback(ctx.get(), PulseAudioSink::subscribeCallback, this);
        pa_operation *subOp = pa_context_subscribe(ctx.get(), PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr);
        if (subOp) {
            pa_operation_unref(subOp);
        } else {
            qCWarning(dragonMediaBackendAudio) << "PulseAudio: pa_context_subscribe failed";
        }
    }

    m_pa->context = std::move(ctx);
    m_pa->contextReady = true;
    cleanupGuard.dismiss();

    qCDebug(dragonMediaBackendAudio) << "PulseAudio connected";
    return {};
}

void PulseAudioSink::disconnectFromServer()
{
    m_pa->contextReady = false;

    if (m_pa->mainloop) {
        ScopedMainloopLock lock(m_pa->mainloop.get());

        // Streams must have been closed (and unregistered) by their owning
        // voices before the connection goes away.
        Q_ASSERT(m_pa->streamsByIndex.empty());
        m_pa->streamsByIndex.clear();

        if (m_pa->context) {
            pa_context_set_state_callback(m_pa->context.get(), nullptr, nullptr);
            pa_context_set_subscribe_callback(m_pa->context.get(), nullptr, nullptr);
            m_pa->context.reset();
        }
    }

    m_pa->mainloop.reset();
}

std::size_t PulseAudioSink::preferredCallbackSamples(int sampleRate, int channels) const
{
    return static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(channels);
}

void PulseAudioSink::registerStream(PulseAudioSinkStream *stream)
{
    ScopedMainloopLock lock(m_pa->mainloop.get());
    m_pa->streamsByIndex.insert_or_assign(stream->sinkInputIndex(), stream);
}

void PulseAudioSink::unregisterStream(PulseAudioSinkStream *stream)
{
    if (!m_pa->mainloop) {
        return;
    }
    ScopedMainloopLock lock(m_pa->mainloop.get());
    if (const auto it = m_pa->streamsByIndex.find(stream->sinkInputIndex()); it != m_pa->streamsByIndex.end() && it->second == stream) {
        m_pa->streamsByIndex.erase(it);
    }
}

std::expected<std::unique_ptr<AudioSinkStream>, QString>
PulseAudioSink::createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit)
{
    if (m_poisoned) {
        return std::unexpected(i18n("PulseAudio audio device is poisoned"));
    }

    qCDebug(dragonMediaBackendAudio) << "PulseAudio createStream" << sampleRate << channels;

    // Session setup can transiently fail server-side: the pulse server (or
    // pipewire-pulse standing in for it, e.g. in CI) may drop the connection
    // or reject CREATE_PLAYBACK_STREAM while it is still tearing down the
    // previous cycle's objects. Retry the full setup before giving up.
    constexpr int kMaxAttempts = 3;
    constexpr auto kRetryBackoffBase = std::chrono::milliseconds(50);

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (attempt > 1) {
            const auto backoff = kRetryBackoffBase * (attempt - 1);
            qCWarning(dragonMediaBackendAudio) << "PulseAudio: stream setup failed, retrying in" << backoff.count() << "ms (attempt" << attempt << "of"
                                               << kMaxAttempts << ")";
            std::this_thread::sleep_for(backoff);
        }

        if (!m_pa->contextReady) {
            if (const ConnectResult connected = connectToServer(); !connected) {
                if (!connected.error().retryable || attempt == kMaxAttempts) {
                    return std::unexpected(connected.error().message);
                }
                continue;
            }
        }

        auto stream = std::make_unique<PulseAudioSinkStream>(*this, sampleRate, channels, renderer);
        if (stream->connectStream(volumeInit)) {
            registerStream(stream.get());
            qCDebug(dragonMediaBackendAudio) << "PulseAudio stream opened, sink_input_index:" << stream->sinkInputIndex();
            return stream;
        }

        // Drop the half-connected stream before checking whether other active
        // streams still depend on the shared PulseAudio context.
        stream.reset();

        bool hasLiveStreams = false;
        if (m_pa && m_pa->mainloop) {
            ScopedMainloopLock lock(m_pa->mainloop.get());
            hasLiveStreams = !m_pa->streamsByIndex.empty();
        }
        if (m_poisoned || hasLiveStreams) {
            return std::unexpected(i18n("PulseAudio: failed to connect stream"));
        }

        disconnectFromServer();

        if (attempt == kMaxAttempts) {
            return std::unexpected(i18n("PulseAudio: failed to connect stream"));
        }
    }

    return std::unexpected(i18n("PulseAudio: failed to connect stream"));
}

void PulseAudioSink::setStreamName(const QString &name)
{
    AudioSink::setStreamName(name);

    if (!m_pa->mainloop || !m_pa->context) {
        return;
    }

    ScopedMainloopLock lock(m_pa->mainloop.get());

    const QString mediaName = resolvedStreamName();
    for (auto &[index, stream] : m_pa->streamsByIndex) {
        Q_UNUSED(index);
        stream->updateStreamNameLocked(mediaName);
    }
}

PulseAudioSinkStream::PulseAudioSinkStream(PulseAudioSink &sink, int sampleRate, int channels, AudioSinkStreamRenderer &renderer)
    : AudioSinkStream(renderer)
    , m_sink(sink)
    , m_sampleRate(sampleRate)
    , m_channels(channels)
    , m_stream(std::make_unique<PaStream>())
{
}

PulseAudioSinkStream::~PulseAudioSinkStream()
{
    std::ignore = close();
}

bool PulseAudioSinkStream::connectStream(const StreamVolumeInit &volumeInit)
{
    m_firstVolumeReportSeen.store(false, std::memory_order_relaxed);
    m_gainSetByPipeline.store(volumeInit.linearGain.has_value(), std::memory_order_relaxed);
    if (volumeInit.linearGain) {
        m_cachedGain.store(*volumeInit.linearGain, std::memory_order_relaxed);
    }
    m_cachedMuted.store(volumeInit.muted, std::memory_order_relaxed);

    if (m_sampleRate <= 0 || m_channels <= 0 || m_channels > static_cast<int>(PA_CHANNELS_MAX)) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: invalid sample spec rate=" << m_sampleRate << "channels=" << m_channels;
        return false;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = static_cast<uint32_t>(m_sampleRate);
    ss.channels = static_cast<uint8_t>(m_channels);
    if (!pa_sample_spec_valid(&ss)) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: invalid sample spec rate=" << m_sampleRate << "channels=" << m_channels;
        return false;
    }

    pa_channel_map channelMap;
    if (!pa_channel_map_init_auto(&channelMap, m_channels, PA_CHANNEL_MAP_DEFAULT)) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: failed to init channel map for" << m_channels << "channels";
        return false;
    }

    const QString mediaName = m_sink.resolvedStreamName();
    const QByteArray mediaNameUtf8 = mediaName.toUtf8();

    pa_proplist *streamProplist = pa_proplist_new();
    if (!streamProplist) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_proplist_new failed";
        return false;
    }
    auto streamProplistGuard = qScopeGuard([streamProplist]() {
        pa_proplist_free(streamProplist);
    });

    pa_proplist_sets(streamProplist, PA_PROP_MEDIA_NAME, mediaNameUtf8.constData());
    pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_NAME, AudioSink::defaultStreamName().toUtf8().constData());
    pa_proplist_sets(streamProplist, PA_PROP_MEDIA_ROLE, "music");
    const QString appId = AudioSink::applicationIconName();
    if (!appId.isEmpty()) {
        const QByteArray appIdUtf8 = appId.toUtf8();
        pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_ICON_NAME, appIdUtf8.constData());
        pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_ID, appIdUtf8.constData());
    }

    {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());

        m_stream->handle.reset(pa_stream_new_with_proplist(m_sink.m_pa->context.get(), mediaNameUtf8.constData(), &ss, &channelMap, streamProplist));
        if (!m_stream->handle) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_stream_new_with_proplist failed:"
                                                << pa_strerror(pa_context_errno(m_sink.m_pa->context.get()));
            return false;
        }

        pa_stream_set_state_callback(m_stream->handle.get(), PulseAudioSinkStream::streamStateCallback, this);
        pa_stream_set_write_callback(m_stream->handle.get(), PulseAudioSinkStream::writeCallback, this);
        pa_stream_set_underflow_callback(m_stream->handle.get(), PulseAudioSinkStream::underflowCallback, this);

        pa_buffer_attr bufferAttr;
        bufferAttr.maxlength = static_cast<uint32_t>(-1);
        bufferAttr.tlength = static_cast<uint32_t>(pa_bytes_per_second(&ss) * kDefaultBufferMs / 1000 * kDefaultTlengthMultiplier);
        bufferAttr.prebuf = 0;
        bufferAttr.minreq = sizeof(float) * 1024;
        bufferAttr.fragsize = sizeof(float) * 1024;

        pa_cvolume cvol;
        const pa_cvolume *volumeArg = nullptr;
        if (volumeInit.linearGain) {
            pa_cvolume_set(&cvol, m_channels, pa_sw_volume_from_linear(*volumeInit.linearGain));
            volumeArg = &cvol;
        }

        const auto streamFlags = static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING
                                                                | (volumeInit.muted ? PA_STREAM_START_MUTED : 0));

        // Mark the stream open BEFORE connecting the playback stream: the
        // first writeCallback can fire from within the
        // pa_threaded_mainloop_wait() below (while we still hold the
        // mainloop lock), and it must observe m_open == true or it drops
        // the very first write request. Error paths below reset the flag.
        m_paused.store(false, std::memory_order_release);
        m_open.store(true, std::memory_order_seq_cst);

        int ret = pa_stream_connect_playback(m_stream->handle.get(), nullptr, &bufferAttr, streamFlags, volumeArg, nullptr);

        if (ret < 0) {
            m_open.store(false, std::memory_order_seq_cst);
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_stream_connect_playback failed:"
                                                << pa_strerror(pa_context_errno(m_sink.m_pa->context.get()));
            return false;
        }

        pa_stream_state_t streamState = pa_stream_get_state(m_stream->handle.get());
        while (PA_STREAM_IS_GOOD(streamState) && streamState != PA_STREAM_READY) {
            pa_threaded_mainloop_wait(m_sink.m_pa->mainloop.get());
            streamState = pa_stream_get_state(m_stream->handle.get());
        }

        if (!PA_STREAM_IS_GOOD(streamState) || streamState != PA_STREAM_READY) {
            m_open.store(false, std::memory_order_seq_cst);
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: stream did not become ready, state:" << streamState
                                                << "context state:" << pa_context_get_state(m_sink.m_pa->context.get())
                                                << "error:" << pa_strerror(pa_context_errno(m_sink.m_pa->context.get()));
            return false;
        }
    }

    m_sinkInputIndex.store(pa_stream_get_index(m_stream->handle.get()), std::memory_order_release);
    {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        requestSinkInputInfo();
    }
    return true;
}

bool PulseAudioSinkStream::closeStream()
{
    m_open.store(false, std::memory_order_seq_cst);
    m_paused.store(false, std::memory_order_release);

    if (!m_sink.m_pa->mainloop || !m_stream->handle) {
        return true;
    }

    // Wait for active callbacks to finish BEFORE tearing down PA objects.
    // Callbacks check the quiesce handshake and bail quickly when close is
    // in progress, so this is bounded.
    m_quiesce.beginClose();
    const bool quiesced = m_quiesce.waitForIdle(CallbackQuiesce::kDefaultCloseTimeout);
    if (!quiesced) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio stream close(): callbacks did not quiesce within" << CallbackQuiesce::kDefaultCloseTimeout.count()
                                            << "ms; leaking the PA stream (zombie state) to avoid a use-after-free";
        m_sink.m_poisoned = true;
        m_sink.unregisterStream(this);
        std::ignore = m_stream.release();
        return false;
    }

    {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());

        // Cancel operations whose dispatch callbacks carry this stream as
        // userdata before freeing it: pa_operation_cancel() unlinks the
        // operation and nulls its callback/userdata, so a late reply will
        // not dispatch into freed memory.
        if (m_infoOp) {
            pa_operation_cancel(m_infoOp);
            pa_operation_unref(m_infoOp);
            m_infoOp = nullptr;
        }
        if (m_drainOp) {
            pa_operation_cancel(m_drainOp);
            pa_operation_unref(m_drainOp);
            m_drainOp = nullptr;
        }

        if (m_stream->handle) {
            pa_stream_set_state_callback(m_stream->handle.get(), nullptr, nullptr);
            pa_stream_set_write_callback(m_stream->handle.get(), nullptr, nullptr);
            pa_stream_set_underflow_callback(m_stream->handle.get(), nullptr, nullptr);
            m_stream->handle.reset();
        }
    }

    m_sink.unregisterStream(this);
    return true;
}

void PulseAudioSinkStream::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_sink.m_pa->mainloop && m_stream->handle) {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        pa_operation *op = pa_stream_cork(m_stream->handle.get(), 1, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void PulseAudioSinkStream::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_sink.m_pa->mainloop && m_stream->handle) {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        pa_operation *op = pa_stream_cork(m_stream->handle.get(), 0, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

bool PulseAudioSinkStream::isPaused() const
{
    return m_paused.load(std::memory_order_acquire);
}

void PulseAudioSinkStream::setGain(float linearGain)
{
    m_gainSetByPipeline.store(true, std::memory_order_relaxed);
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    if (m_sink.m_pa->mainloop && m_stream->handle && m_open.load(std::memory_order_acquire)) {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        applyVolume(linearGain);
    }
}

void PulseAudioSinkStream::applyVolume(float linearGain)
{
    if (!m_stream->handle || !m_sink.m_pa->context) {
        return;
    }

    pa_cvolume cvol;
    pa_cvolume_set(&cvol, static_cast<unsigned>(m_channels), pa_sw_volume_from_linear(linearGain));
    pa_operation *op = pa_context_set_sink_input_volume(m_sink.m_pa->context.get(), m_sinkInputIndex.load(std::memory_order_acquire), &cvol, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
}

void PulseAudioSinkStream::setMuted(bool muted)
{
    m_cachedMuted.store(muted, std::memory_order_relaxed);

    if (m_sink.m_pa->context && m_sink.m_pa->mainloop && m_open.load(std::memory_order_acquire)) {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        pa_operation *op =
            pa_context_set_sink_input_mute(m_sink.m_pa->context.get(), m_sinkInputIndex.load(std::memory_order_acquire), muted ? 1 : 0, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void PulseAudioSinkStream::clearStream()
{
    if (m_sink.m_pa->mainloop && m_stream->handle) {
        ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());
        pa_operation *op = pa_stream_flush(m_stream->handle.get(), nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void PulseAudioSinkStream::updateStreamNameLocked(const QString &mediaName)
{
    if (!m_stream->handle) {
        return;
    }

    pa_proplist *proplist = pa_proplist_new();
    if (!proplist) {
        return;
    }

    pa_proplist_sets(proplist, PA_PROP_MEDIA_NAME, mediaName.toUtf8().constData());

    pa_operation *op = pa_stream_proplist_update(m_stream->handle.get(), PA_UPDATE_REPLACE, proplist, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
    pa_proplist_free(proplist);
}

std::optional<std::chrono::microseconds> PulseAudioSinkStream::queuedDurationLocked() const
{
    pa_usec_t latency = 0;
    int negative = 0;
    if (pa_stream_get_latency(m_stream->handle.get(), &latency, &negative) != 0 || negative) {
        return std::nullopt;
    }
    return std::chrono::microseconds{static_cast<qint64>(latency)};
}

qint64 PulseAudioSinkStream::deviceQueuedSamples() const
{
    if (!m_sink.m_pa->mainloop || !m_stream->handle || !m_open.load(std::memory_order_acquire)) {
        return 0;
    }

    if (m_channels <= 0 || m_sampleRate <= 0) {
        return 0;
    }

    ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());

    const auto queuedUs = queuedDurationLocked();
    if (!queuedUs) {
        return 0;
    }

    return queuedUs->count() * m_sampleRate * m_channels / 1000000;
}

int PulseAudioSinkStream::audioBufferFrames() const
{
    if (!m_sink.m_pa->mainloop || !m_stream->handle || !m_open.load(std::memory_order_acquire)) {
        return -1;
    }

    if (m_sampleRate <= 0) {
        return -1;
    }

    ScopedMainloopLock lock(m_sink.m_pa->mainloop.get());

    const auto queuedUs = queuedDurationLocked();
    if (!queuedUs) {
        return -1;
    }

    return static_cast<int>(queuedUs->count() * m_sampleRate / 1000000);
}

std::optional<std::chrono::microseconds> PulseAudioSinkStream::audioBufferDuration() const
{
    const int frames = audioBufferFrames();
    if (frames <= 0 || m_sampleRate <= 0) {
        return std::nullopt;
    }

    return std::chrono::microseconds{(static_cast<qint64>(frames) * 1000000) / m_sampleRate};
}

void PulseAudioSinkStream::writeCallback(pa_stream *s, size_t nbytes, void *userdata)
{
    auto *self = static_cast<PulseAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing()) {
        return;
    }

    if (!self->m_open.load(std::memory_order_seq_cst)) {
        return;
    }

    if (nbytes == 0) {
        return;
    }

    const int channels = self->m_channels;
    if (channels <= 0) {
        return;
    }

    const size_t floatsNeeded = (nbytes / sizeof(float)) / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    if (floatsNeeded == 0) {
        return;
    }

    pa_usec_t latency = 0;
    int negative = 0;
    if (pa_stream_get_latency(s, &latency, &negative) == 0) {
        if (negative) {
            latency = 0;
        }
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(latency);

    auto pcm = self->renderer().render(floatsNeeded, pts);

    if (!pcm.empty()) {
        size_t bytes = pcm.size() * sizeof(float);
        int ret = pa_stream_write(s, pcm.data(), bytes, nullptr, 0, PA_SEEK_RELATIVE);
        if (ret < 0) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio pa_stream_write failed:" << pa_strerror(pa_context_errno(self->m_sink.m_pa->context.get()));
        }
    } else if (self->renderer().tryClaimDrained()) {
        // Keep the operation referenced so closeStream() can cancel it: its
        // dispatch callback carries this stream as userdata.
        if (self->m_drainOp) {
            pa_operation_cancel(self->m_drainOp);
            pa_operation_unref(self->m_drainOp);
            self->m_drainOp = nullptr;
        }
        self->m_drainOp = pa_stream_drain(s, PulseAudioSinkStream::drainCallback, self);
        if (!self->m_drainOp) {
            qCWarning(dragonMediaBackendAudio) << "PulseAudio pa_stream_drain failed:" << pa_strerror(pa_context_errno(self->m_sink.m_pa->context.get()));
        }
    } else if (!self->renderer().endOfStreamArmed()) {
        const size_t silenceBytes = floatsNeeded * sizeof(float);
        const std::size_t written = fillStreamWithSilence(
            silenceBytes,
            self->m_channels,
            [s](void **buf, std::size_t *bytes) {
                return pa_stream_begin_write(s, buf, bytes);
            },
            [s](void *buf, std::size_t bytes) {
                return pa_stream_write(s, buf, bytes, nullptr, 0, PA_SEEK_RELATIVE);
            },
            [s]() {
                pa_stream_cancel_write(s);
            });
        if (written < silenceBytes) {
            qCWarning(dragonMediaBackendAudio) << "PulseAudio silence fill incomplete:" << written << "of" << silenceBytes
                                               << pa_strerror(pa_context_errno(self->m_sink.m_pa->context.get()));
        }
    }
}

void PulseAudioSinkStream::streamStateCallback(pa_stream *s, void *userdata)
{
    auto *self = static_cast<PulseAudioSinkStream *>(userdata);
    if (!self || !s) {
        return;
    }
    const pa_stream_state_t state = pa_stream_get_state(s);
    if (state == PA_STREAM_FAILED) {
        const char *err = (self->m_sink.m_pa && self->m_sink.m_pa->context) ? pa_strerror(pa_context_errno(self->m_sink.m_pa->context.get())) : nullptr;
        const QString errorMsg = err ? QString::fromUtf8(err) : QStringLiteral("PulseAudio stream failed");
        qCCritical(dragonMediaBackendAudio) << "PulseAudio stream error state:" << errorMsg;
        if (self->m_open.load(std::memory_order_acquire)) {
            QMetaObject::invokeMethod(
                self,
                [self, errorMsg]() {
                    Q_EMIT self->errorOccurred(errorMsg);
                },
                Qt::QueuedConnection);
        }
    }
    if (self->m_sink.m_pa && self->m_sink.m_pa->mainloop) {
        pa_threaded_mainloop_signal(self->m_sink.m_pa->mainloop.get(), 0);
    }
}

void PulseAudioSinkStream::drainCallback(pa_stream *s, int success, void *userdata)
{
    Q_UNUSED(s);
    auto *self = static_cast<PulseAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }
    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (!success) {
        qCWarning(dragonMediaBackendAudio) << "PulseAudio stream drain failed";
    }
    if (self->m_drainOp) {
        pa_operation_unref(self->m_drainOp);
        self->m_drainOp = nullptr;
    }
    if (guard.closing() || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }
    QMetaObject::invokeMethod(
        self,
        [self]() {
            if (self->m_open.load(std::memory_order_acquire) && self->renderer().takeDrainedEmission()) {
                Q_EMIT self->drained();
            }
        },
        Qt::QueuedConnection);
}

void PulseAudioSinkStream::underflowCallback(pa_stream *s, void *userdata)
{
    Q_UNUSED(s);
    Q_UNUSED(userdata);
    qCDebug(dragonMediaBackendAudio) << "PulseAudio underflow detected";
}

void PulseAudioSinkStream::sinkInputInfoCallback(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<PulseAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }

    if (!info || eol != 0) {
        // Final completion dispatch of the info operation: release the
        // reference requestSinkInputInfo() took so closeStream() could
        // cancel it.
        if (self->m_infoOp) {
            pa_operation_unref(self->m_infoOp);
            self->m_infoOp = nullptr;
        }
        return;
    }

    // Quiesce handshake with closeStream(): bail out before touching sink
    // state when close is in progress.
    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing() || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    self->handleSinkInputInfo(info);
}

void PulseAudioSinkStream::handleSinkInputInfo(const pa_sink_input_info *info)
{
    if (info->has_volume) {
        const pa_volume_t avg = pa_cvolume_avg(&info->volume);
        const float linearGain = static_cast<float>(pa_sw_volume_to_linear(avg));

        const bool isFirstReport = !m_firstVolumeReportSeen.exchange(true, std::memory_order_relaxed);
        const bool neverSetGain = !m_gainSetByPipeline.load(std::memory_order_relaxed);

        float cached = m_cachedGain.load(std::memory_order_relaxed);
        if (qAbs(linearGain - cached) >= 0.001f) {
            m_cachedGain.store(linearGain, std::memory_order_relaxed);

            if (isFirstReport && neverSetGain) {
                qCDebug(dragonMediaBackendAudio) << "PulseAudio stream-restore volume, linearGain:" << linearGain;
                QMetaObject::invokeMethod(
                    this,
                    [this, linearGain]() {
                        Q_EMIT externalVolumeRestored(linearGain);
                    },
                    Qt::QueuedConnection);
            } else {
                qCDebug(dragonMediaBackendAudio) << "PulseAudio external volume change, linearGain:" << linearGain;
                QMetaObject::invokeMethod(
                    this,
                    [this, linearGain]() {
                        Q_EMIT externalVolumeChanged(linearGain);
                    },
                    Qt::QueuedConnection);
            }
        }
    }

    const bool muted = info->mute != 0;
    if (muted != m_cachedMuted.load(std::memory_order_relaxed)) {
        m_cachedMuted.store(muted, std::memory_order_relaxed);

        qCDebug(dragonMediaBackendAudio) << "PulseAudio external mute change, muted:" << muted;
        QMetaObject::invokeMethod(
            this,
            [this, muted]() {
                Q_EMIT externalMutedChanged(muted);
            },
            Qt::QueuedConnection);
    }
}

void PulseAudioSinkStream::requestSinkInputInfo()
{
    if (!m_sink.m_pa->context || !m_sink.m_pa->mainloop || !m_stream->handle) {
        return;
    }
    if (m_infoOp) {
        pa_operation_cancel(m_infoOp);
        pa_operation_unref(m_infoOp);
        m_infoOp = nullptr;
    }
    // Keep the operation referenced so closeStream() can cancel it: its
    // dispatch callback carries this stream as userdata.
    m_infoOp = pa_context_get_sink_input_info(m_sink.m_pa->context.get(),
                                              m_sinkInputIndex.load(std::memory_order_acquire),
                                              PulseAudioSinkStream::sinkInputInfoCallback,
                                              this);
}

void PulseAudioSink::contextStateCallback(pa_context *c, void *userdata)
{
    auto *self = static_cast<PulseAudioSink *>(userdata);
    if (!self || !c) {
        return;
    }
    const pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_FAILED) {
        const char *err = pa_strerror(pa_context_errno(c));
        const QString errorMsg = err ? QString::fromUtf8(err) : QStringLiteral("PulseAudio context failed");
        qCCritical(dragonMediaBackendAudio) << "PulseAudio context error state:" << errorMsg;
        QMetaObject::invokeMethod(
            self,
            [self, errorMsg]() {
                Q_EMIT self->errorOccurred(errorMsg);
            },
            Qt::QueuedConnection);
    }
    if (self->m_pa && self->m_pa->mainloop) {
        pa_threaded_mainloop_signal(self->m_pa->mainloop.get(), 0);
    }
}

void PulseAudioSink::subscribeCallback(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<PulseAudioSink *>(userdata);
    if (!self) {
        return;
    }

    const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        return;
    }

    const auto eventType = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (eventType == PA_SUBSCRIPTION_EVENT_REMOVE) {
        return;
    }

    // Subscription callbacks already run with the mainloop lock held by the
    // PA thread: never take it again from here (the map is guarded by that
    // very lock).
    const auto it = self->m_pa->streamsByIndex.find(idx);
    if (it == self->m_pa->streamsByIndex.end()) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PulseAudio sink_input" << idx << "changed, fetching info";
    it->second->requestSinkInputInfo();
}

#include "pulseaudiosink.moc"
