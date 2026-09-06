/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpulseaudiosink.h"

#include "dragonmediabackend_audio_logging.h"

#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(DragonPulseAudioSink, "pulseaudio_sink.json")

#include <QGuiApplication>
#include <QScopeGuard>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>

#include <chrono>
#include <span>

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
using StreamPtr = std::unique_ptr<pa_stream, StreamDeleter>;

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

struct DragonPulseAudioSink::PaState {
    MainloopPtr mainloop;
    ContextPtr context;
    StreamPtr stream;
    uint32_t sinkInputIndex = static_cast<uint32_t>(-1);
    bool contextReady = false;
};

DragonPulseAudioSink::DragonPulseAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
    , m_pa(std::make_unique<PaState>())
{
    Q_UNUSED(args);
    qCDebug(dragonMediaBackendAudio) << "PulseAudio audio sink created";
}

DragonPulseAudioSink::~DragonPulseAudioSink()
{
    DragonPulseAudioSink::close();
}

bool DragonPulseAudioSink::probe()
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

    {
        ScopedMainloopLock lock(ml.get());
        pa_context_set_state_callback(ctx.get(), probeContextStateCallback, ml.get());

        if (pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() pa_context_connect failed";
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

        bool alive = PA_CONTEXT_IS_GOOD(state) && (state == PA_CONTEXT_READY);
        if (alive) {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() daemon reachable";
        } else {
            qCDebug(dragonMediaBackendAudio) << "PulseAudio probe() daemon unreachable";
        }
        return alive;
    }
}

void DragonPulseAudioSink::contextStateCallback(pa_context *c, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    pa_threaded_mainloop_signal(self->m_pa->mainloop.get(), 0);
}

void DragonPulseAudioSink::streamStateCallback(pa_stream *s, void *userdata)
{
    Q_UNUSED(s);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    pa_threaded_mainloop_signal(self->m_pa->mainloop.get(), 0);
}

void DragonPulseAudioSink::writeCallback(pa_stream *s, size_t nbytes, void *userdata)
{
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    self->m_activeCallbacks.fetch_add(1, std::memory_order_relaxed);

    auto cleanupGuard = qScopeGuard([self]() noexcept {
        self->m_activeCallbacks.fetch_sub(1, std::memory_order_relaxed);
        self->m_callbackDoneCv.notify_one();
    });

    if (!self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    if (nbytes == 0) {
        return;
    }

    const int channels = self->currentChannels();

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

    auto pcm = self->processAudioCallback(floatsNeeded, pts);

    if (!pcm.empty()) {
        size_t bytes = pcm.size() * sizeof(float);
        int ret = pa_stream_write(s, pcm.data(), bytes, nullptr, 0, PA_SEEK_RELATIVE);
        if (ret < 0) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio pa_stream_write failed:" << pa_strerror(pa_context_errno(self->m_pa->context.get()));
        }
    } else if (self->m_drain.tryClaimDrain() && self->m_drain.claimIsCurrent()) {
        pa_operation *op = pa_stream_drain(s, DragonPulseAudioSink::drainCallback, self);
        if (!op) {
            qCWarning(dragonMediaBackendAudio) << "PulseAudio pa_stream_drain failed:" << pa_strerror(pa_context_errno(self->m_pa->context.get()));
        }
    } else if (!self->m_drain.decodeFinished()) {
        thread_local std::vector<float> silence;
        silence.resize(floatsNeeded, 0.0f);
        pa_stream_write(s, silence.data(), silence.size() * sizeof(float), nullptr, 0, PA_SEEK_RELATIVE);
    }
}

bool DragonPulseAudioSink::connectToServer()
{
    MainloopPtr ml(pa_threaded_mainloop_new());
    if (!ml) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_threaded_mainloop_new failed";
        Q_EMIT errorOccurred(i18n("PulseAudio: failed to create mainloop"));
        return false;
    }

    pa_threaded_mainloop_set_name(ml.get(), "dragon-pa");

    if (pa_threaded_mainloop_start(ml.get()) < 0) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_threaded_mainloop_start failed";
        Q_EMIT errorOccurred(i18n("PulseAudio: failed to start mainloop"));
        return false;
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
        Q_EMIT errorOccurred(i18n("PulseAudio: failed to get mainloop API"));
        return false;
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

        const QString appId = applicationIconName();
        if (!appId.isEmpty()) {
            const QByteArray appIdUtf8 = appId.toUtf8();
            pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_ICON_NAME, appIdUtf8.constData());
            pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_ID, appIdUtf8.constData());
        }
    }

    ContextPtr ctx(pa_context_new_with_proplist(api, defaultStreamName().toUtf8().constData(), rawProplist));
    if (!ctx) {
        qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_context_new_with_proplist failed";
        Q_EMIT errorOccurred(i18n("PulseAudio: failed to create context"));
        return false;
    }

    {
        ScopedMainloopLock lock(m_pa->mainloop.get());

        pa_context_set_state_callback(ctx.get(), DragonPulseAudioSink::contextStateCallback, this);

        if (pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_context_connect failed";
            Q_EMIT errorOccurred(i18n("PulseAudio: failed to connect context"));
            return false;
        }

        pa_context_state_t state = pa_context_get_state(ctx.get());
        while (PA_CONTEXT_IS_GOOD(state) && state != PA_CONTEXT_READY) {
            pa_threaded_mainloop_wait(m_pa->mainloop.get());
            state = pa_context_get_state(ctx.get());
        }

        if (!PA_CONTEXT_IS_GOOD(state) || state != PA_CONTEXT_READY) {
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: context did not become ready:" << state;
            Q_EMIT errorOccurred(i18n("PulseAudio: context not ready"));
            return false;
        }

        pa_context_set_subscribe_callback(ctx.get(), DragonPulseAudioSink::subscribeCallback, this);
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
    return true;
}

void DragonPulseAudioSink::resetStreamLocked()
{
    if (!m_pa->stream) {
        return;
    }
    pa_stream_set_state_callback(m_pa->stream.get(), nullptr, nullptr);
    pa_stream_set_write_callback(m_pa->stream.get(), nullptr, nullptr);
    pa_stream_set_underflow_callback(m_pa->stream.get(), nullptr, nullptr);
    m_pa->stream.reset();
}

void DragonPulseAudioSink::disconnectFromServer()
{
    m_pa->contextReady = false;

    if (m_pa->mainloop) {
        ScopedMainloopLock lock(m_pa->mainloop.get());

        resetStreamLocked();

        if (m_pa->context) {
            pa_context_set_subscribe_callback(m_pa->context.get(), nullptr, nullptr);
            m_pa->context.reset();
        }
    }

    m_pa->mainloop.reset();
}

void DragonPulseAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendAudio) << "PulseAudio open" << sampleRate << channels;

    if (m_open.load(std::memory_order_acquire)) {
        qCDebug(dragonMediaBackendAudio) << "open() called while already open closing old session";
        close();
    }

    setFormat(sampleRate, channels);
    reset();

    m_lastSampleRate = sampleRate;
    m_lastChannels = channels;

    preAllocateCallbackBuffer(static_cast<size_t>(sampleRate) * static_cast<size_t>(channels));

    if (!connectToServer()) {
        return;
    }

    QString pendingError;

    auto openStream = [&]() -> bool {
        pa_sample_spec ss;
        ss.format = PA_SAMPLE_FLOAT32LE;
        ss.rate = static_cast<uint32_t>(sampleRate);
        ss.channels = static_cast<uint8_t>(channels);

        pa_channel_map channelMap;
        if (!pa_channel_map_init_auto(&channelMap, channels, PA_CHANNEL_MAP_DEFAULT)) {
            pendingError = i18n("PulseAudio: failed to init channel map");
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: failed to init channel map for" << channels << "channels";
            return false;
        }

        const QString mediaName = resolvedStreamName();
        const QByteArray mediaNameUtf8 = mediaName.toUtf8();

        pa_proplist *streamProplist = pa_proplist_new();
        if (!streamProplist) {
            pendingError = i18n("PulseAudio: failed to create stream proplist");
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_proplist_new failed";
            return false;
        }
        auto streamProplistGuard = qScopeGuard([streamProplist]() {
            pa_proplist_free(streamProplist);
        });

        pa_proplist_sets(streamProplist, PA_PROP_MEDIA_NAME, mediaNameUtf8.constData());
        pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_NAME, defaultStreamName().toUtf8().constData());
        pa_proplist_sets(streamProplist, PA_PROP_MEDIA_ROLE, "music");
        const QString appId = applicationIconName();
        if (!appId.isEmpty()) {
            const QByteArray appIdUtf8 = appId.toUtf8();
            pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_ICON_NAME, appIdUtf8.constData());
            pa_proplist_sets(streamProplist, PA_PROP_APPLICATION_ID, appIdUtf8.constData());
        }

        m_pa->stream.reset(pa_stream_new_with_proplist(m_pa->context.get(), mediaNameUtf8.constData(), &ss, &channelMap, streamProplist));
        if (!m_pa->stream) {
            pendingError = i18n("PulseAudio: failed to create stream");
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_stream_new_with_proplist failed:" << pa_strerror(pa_context_errno(m_pa->context.get()));
            return false;
        }

        pa_stream_set_state_callback(m_pa->stream.get(), DragonPulseAudioSink::streamStateCallback, this);
        pa_stream_set_write_callback(m_pa->stream.get(), DragonPulseAudioSink::writeCallback, this);
        pa_stream_set_underflow_callback(m_pa->stream.get(), DragonPulseAudioSink::underflowCallback, this);

        pa_buffer_attr bufferAttr;
        bufferAttr.maxlength = static_cast<uint32_t>(-1);
        bufferAttr.tlength = static_cast<uint32_t>(pa_bytes_per_second(&ss) * kDefaultBufferMs / 1000 * kDefaultTlengthMultiplier);
        bufferAttr.prebuf = 0;
        bufferAttr.minreq = sizeof(float) * 1024;
        bufferAttr.fragsize = sizeof(float) * 1024;

        pa_cvolume cvol;
        pa_cvolume_set(&cvol, channels, pa_sw_volume_from_linear(m_cachedGain.load(std::memory_order_relaxed)));

        int ret =
            pa_stream_connect_playback(m_pa->stream.get(),
                                       nullptr,
                                       &bufferAttr,
                                       static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING),
                                       &cvol,
                                       nullptr);

        if (ret < 0) {
            pendingError = i18n("PulseAudio: failed to connect stream");
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: pa_stream_connect_playback failed:" << pa_strerror(pa_context_errno(m_pa->context.get()));
            return false;
        }

        pa_stream_state_t streamState = pa_stream_get_state(m_pa->stream.get());
        while (PA_STREAM_IS_GOOD(streamState) && streamState != PA_STREAM_READY) {
            pa_threaded_mainloop_wait(m_pa->mainloop.get());
            streamState = pa_stream_get_state(m_pa->stream.get());
        }

        if (!PA_STREAM_IS_GOOD(streamState) || streamState != PA_STREAM_READY) {
            pendingError = i18n("PulseAudio: stream not ready");
            qCCritical(dragonMediaBackendAudio) << "PulseAudio: stream did not become ready";
            return false;
        }

        m_pa->sinkInputIndex = pa_stream_get_index(m_pa->stream.get());
        return true;
    };

    bool ok = false;
    {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        ok = openStream();
        if (!ok) {
            resetStreamLocked();
        }
    } // ScopedMainloopLock released here

    if (!ok) {
        if (!pendingError.isEmpty()) {
            Q_EMIT errorOccurred(pendingError);
        }
        disconnectFromServer();
        return;
    }

    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    qCDebug(dragonMediaBackendAudio) << "PulseAudio stream opened, sink_input_index:" << m_pa->sinkInputIndex;
}

void DragonPulseAudioSink::close()
{
    qCDebug(dragonMediaBackendAudio) << "PulseAudio close()";

    m_open.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    // Wait for active callbacks to finish BEFORE tearing down PA objects.
    // Callbacks check m_open and bail quickly when false, so this is bounded.
    {
        std::unique_lock lock(m_callbackDoneMutex);
        m_callbackDoneCv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return m_activeCallbacks.load(std::memory_order_acquire) == 0;
        });
    }

    disconnectFromServer();

    qCDebug(dragonMediaBackendAudio) << "PulseAudio close() complete";
}

void DragonPulseAudioSink::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        pa_operation *op = pa_stream_cork(m_pa->stream.get(), 1, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void DragonPulseAudioSink::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        pa_operation *op = pa_stream_cork(m_pa->stream.get(), 0, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void DragonPulseAudioSink::setGain(float linearGain)
{
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    if (m_pa->stream && m_pa->mainloop && m_open.load(std::memory_order_acquire)) {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        applyVolume(linearGain);
    }
}

void DragonPulseAudioSink::applyVolume(float linearGain)
{
    if (!m_pa->stream) {
        return;
    }

    const int channels = currentChannels();
    if (channels <= 0) {
        return;
    }

    pa_cvolume cvol;
    pa_cvolume_set(&cvol, static_cast<unsigned>(channels), pa_sw_volume_from_linear(linearGain));
    pa_operation *op = pa_context_set_sink_input_volume(m_pa->context.get(), m_pa->sinkInputIndex, &cvol, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
}

void DragonPulseAudioSink::setMuted(bool muted)
{
    m_cachedMuted.store(muted, std::memory_order_relaxed);

    if (m_pa->context && m_pa->mainloop && m_open.load(std::memory_order_acquire)) {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        pa_operation *op = pa_context_set_sink_input_mute(m_pa->context.get(), m_pa->sinkInputIndex, muted ? 1 : 0, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }

    DragonAudioSink::setMuted(muted);
}

void DragonPulseAudioSink::clearStream()
{
    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop.get());
        pa_operation *op = pa_stream_flush(m_pa->stream.get(), nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

qint64 DragonPulseAudioSink::deviceQueuedSamples() const
{
    if (!m_pa->mainloop || !m_pa->stream || !m_open.load(std::memory_order_acquire)) {
        return 0;
    }

    ScopedMainloopLock lock(m_pa->mainloop.get());

    const pa_timing_info *ti = pa_stream_get_timing_info(m_pa->stream.get());
    if (!ti) {
        return 0;
    }

    const qint64 bytesQueued = ti->write_index - ti->read_index;
    if (bytesQueued < 0) {
        return 0;
    }

    return bytesQueued / static_cast<qint64>(sizeof(float));
}

int DragonPulseAudioSink::audioBufferFrames() const
{
    if (!m_pa->mainloop || !m_pa->stream || !m_open.load(std::memory_order_acquire)) {
        return -1;
    }

    const int channels = currentChannels();
    if (channels <= 0) {
        return -1;
    }

    ScopedMainloopLock lock(m_pa->mainloop.get());

    const pa_timing_info *ti = pa_stream_get_timing_info(m_pa->stream.get());
    if (!ti) {
        return -1;
    }

    const qint64 bytesQueued = ti->write_index - ti->read_index;
    if (bytesQueued < 0) {
        return 0;
    }

    return static_cast<int>(bytesQueued / static_cast<qint64>(channels * sizeof(float)));
}

int DragonPulseAudioSink::audioBufferUs() const
{
    const int frames = audioBufferFrames();
    if (frames <= 0 || currentSampleRate() <= 0) {
        return -1;
    }

    return static_cast<int>((static_cast<qint64>(frames) * 1000000) / currentSampleRate());
}

bool DragonPulseAudioSink::isDeviceOpen() const
{
    return m_open.load(std::memory_order_acquire);
}

bool DragonPulseAudioSink::isPaused() const
{
    return m_paused.load(std::memory_order_acquire);
}

void DragonPulseAudioSink::setStreamName(const QString &name)
{
    DragonAudioSink::setStreamName(name);

    if (!m_pa->mainloop || !m_pa->stream || !m_open.load(std::memory_order_acquire)) {
        return;
    }

    ScopedMainloopLock lock(m_pa->mainloop.get());

    pa_proplist *proplist = pa_proplist_new();
    if (!proplist) {
        return;
    }

    const QString mediaName = resolvedStreamName();
    pa_proplist_sets(proplist, PA_PROP_MEDIA_NAME, mediaName.toUtf8().constData());

    pa_operation *op = pa_stream_proplist_update(m_pa->stream.get(), PA_UPDATE_REPLACE, proplist, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
    pa_proplist_free(proplist);
}

void DragonPulseAudioSink::drainCallback(pa_stream *s, int success, void *userdata)
{
    Q_UNUSED(s);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    if (!success) {
        qCWarning(dragonMediaBackendAudio) << "PulseAudio stream drain failed";
    }
    if (!self->m_drain.claimIsCurrent()) {
        return;
    }
    self->m_drain.consumeEmission();
    Q_EMIT self->drained();
}

void DragonPulseAudioSink::underflowCallback(pa_stream *s, void *userdata)
{
    Q_UNUSED(s);
    Q_UNUSED(userdata);
    qCDebug(dragonMediaBackendAudio) << "PulseAudio underflow detected";
}

void DragonPulseAudioSink::subscribeCallback(pa_context *c, pa_subscription_event_type_t type, uint32_t idx, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    if (!self) {
        return;
    }

    const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (facility != PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        return;
    }

    if (idx != self->m_pa->sinkInputIndex) {
        return;
    }

    const auto eventType = type & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
    if (eventType == PA_SUBSCRIPTION_EVENT_REMOVE) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PulseAudio sink_input" << idx << "changed, fetching info";
    self->requestSinkInputInfo();
}

void DragonPulseAudioSink::sinkInputInfoCallback(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    if (!self || !info || eol != 0) {
        return;
    }

    if (info->has_volume) {
        const pa_volume_t avg = pa_cvolume_avg(&info->volume);
        const float linearGain = static_cast<float>(pa_sw_volume_to_linear(avg));

        float cached = self->m_cachedGain.load(std::memory_order_relaxed);
        if (qAbs(linearGain - cached) >= 0.001f) {
            self->m_cachedGain.store(linearGain, std::memory_order_relaxed);

            qCDebug(dragonMediaBackendAudio) << "PulseAudio external volume change, linearGain:" << linearGain;
            QMetaObject::invokeMethod(
                self,
                [self, linearGain]() {
                    self->onExternalVolumeChanged(linearGain);
                },
                Qt::QueuedConnection);
        }
    }

    const bool muted = info->mute != 0;
    if (muted != self->m_cachedMuted.load(std::memory_order_relaxed)) {
        self->m_cachedMuted.store(muted, std::memory_order_relaxed);

        qCDebug(dragonMediaBackendAudio) << "PulseAudio external mute change, muted:" << muted;
        QMetaObject::invokeMethod(
            self,
            [self, muted]() {
                self->setMuted(muted);
            },
            Qt::QueuedConnection);
    }
}

void DragonPulseAudioSink::requestSinkInputInfo()
{
    if (!m_pa->context || !m_pa->mainloop) {
        return;
    }
    pa_operation *op = pa_context_get_sink_input_info(m_pa->context.get(), m_pa->sinkInputIndex, DragonPulseAudioSink::sinkInputInfoCallback, this);
    if (op) {
        pa_operation_unref(op);
    }
}

#include "dragonpulseaudiosink.moc"
