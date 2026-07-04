/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpulseaudiosink.h"

#include "dragonmultimedia_audio_logging.h"

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

using MainloopPtr = std::unique_ptr<pa_threaded_mainloop, MainloopDeleter>;
using ContextPtr = std::unique_ptr<pa_context, ContextDeleter>;

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

DragonPulseAudioSink::DragonPulseAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
    , m_pa(std::make_unique<PaState>())
{
    Q_UNUSED(args);
    qCDebug(dragonMultimediaAudio) << "PulseAudio audio sink created";
}

DragonPulseAudioSink::~DragonPulseAudioSink()
{
    DragonPulseAudioSink::close();
}

bool DragonPulseAudioSink::probe()
{
    qCDebug(dragonMultimediaAudio) << "PulseAudio probe() checking daemon connectivity";

    MainloopPtr ml(pa_threaded_mainloop_new());
    if (!ml) {
        qCDebug(dragonMultimediaAudio) << "PulseAudio probe() pa_threaded_mainloop_new failed";
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(ml.get());
    ContextPtr ctx(pa_context_new(api, "dragon-probe"));
    if (!ctx) {
        qCDebug(dragonMultimediaAudio) << "PulseAudio probe() pa_context_new failed";
        return false;
    }

    if (pa_threaded_mainloop_start(ml.get()) < 0) {
        qCDebug(dragonMultimediaAudio) << "PulseAudio probe() pa_threaded_mainloop_start failed";
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
            qCDebug(dragonMultimediaAudio) << "PulseAudio probe() pa_context_connect failed";
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
                qCDebug(dragonMultimediaAudio) << "PulseAudio probe() timed out waiting for context";
                break;
            }
        }

        bool alive = PA_CONTEXT_IS_GOOD(state) && (state == PA_CONTEXT_READY);
        if (alive) {
            qCDebug(dragonMultimediaAudio) << "PulseAudio probe() daemon reachable";
        } else {
            qCDebug(dragonMultimediaAudio) << "PulseAudio probe() daemon unreachable";
        }
        return alive;
    }
}

void DragonPulseAudioSink::contextStateCallback(pa_context *c, void *userdata)
{
    Q_UNUSED(c);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    pa_threaded_mainloop_signal(self->m_pa->mainloop, 0);
}

void DragonPulseAudioSink::streamStateCallback(pa_stream *s, void *userdata)
{
    Q_UNUSED(s);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    pa_threaded_mainloop_signal(self->m_pa->mainloop, 0);
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
            qCCritical(dragonMultimediaAudio) << "PulseAudio pa_stream_write failed:" << pa_strerror(pa_context_errno(self->m_pa->context));
        }
    } else {
        if (self->m_decodeFinished.load(std::memory_order_acquire) && !self->m_drainRequested.exchange(true, std::memory_order_acq_rel)) {
            pa_operation *op = pa_stream_drain(s, DragonPulseAudioSink::drainCallback, self);
            if (op) {
                pa_operation_unref(op);
            } else {
                qCWarning(dragonMultimediaAudio) << "PulseAudio pa_stream_drain failed:" << pa_strerror(pa_context_errno(self->m_pa->context));
            }
        } else if (!self->m_decodeFinished.load(std::memory_order_acquire)) {
            thread_local std::vector<float> silence;
            silence.resize(floatsNeeded, 0.0f);
            pa_stream_write(s, silence.data(), silence.size() * sizeof(float), nullptr, 0, PA_SEEK_RELATIVE);
        }
    }
}

bool DragonPulseAudioSink::connectToServer()
{
    MainloopPtr ml(pa_threaded_mainloop_new());
    if (!ml) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_threaded_mainloop_new failed";
        Q_EMIT errorOccurred(u"PulseAudio: failed to create mainloop"_s);
        return false;
    }

    pa_threaded_mainloop_set_name(ml.get(), "dragon-pa");

    if (pa_threaded_mainloop_start(ml.get()) < 0) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_threaded_mainloop_start failed";
        Q_EMIT errorOccurred(u"PulseAudio: failed to start mainloop"_s);
        return false;
    }

    // From this point onward the PA thread may signal, so publish the mainloop
    // pointer that the state callback uses.
    m_pa->mainloop = ml.get();
    ml.release();

    auto cleanupGuard = qScopeGuard([this]() {
        disconnectFromServer();
    });

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(m_pa->mainloop);
    if (!api) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_threaded_mainloop_get_api returned null";
        Q_EMIT errorOccurred(u"PulseAudio: failed to get mainloop API"_s);
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
        pa_proplist_sets(rawProplist, PA_PROP_APPLICATION_NAME, appName.isEmpty() ? "DragonMultimedia" : appName.toUtf8().constData());
        pa_proplist_sets(rawProplist, PA_PROP_MEDIA_ROLE, "music");
    }

    ContextPtr ctx(pa_context_new_with_proplist(api, m_streamName.empty() ? "DragonMultimedia" : m_streamName.c_str(), rawProplist));
    if (!ctx) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_context_new_with_proplist failed";
        Q_EMIT errorOccurred(u"PulseAudio: failed to create context"_s);
        return false;
    }

    {
        ScopedMainloopLock lock(m_pa->mainloop);

        pa_context_set_state_callback(ctx.get(), DragonPulseAudioSink::contextStateCallback, this);

        if (pa_context_connect(ctx.get(), nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
            qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_context_connect failed";
            Q_EMIT errorOccurred(u"PulseAudio: failed to connect context"_s);
            return false;
        }

        pa_context_state_t state = pa_context_get_state(ctx.get());
        while (PA_CONTEXT_IS_GOOD(state) && state != PA_CONTEXT_READY) {
            pa_threaded_mainloop_wait(m_pa->mainloop);
            state = pa_context_get_state(ctx.get());
        }

        if (!PA_CONTEXT_IS_GOOD(state) || state != PA_CONTEXT_READY) {
            qCCritical(dragonMultimediaAudio) << "PulseAudio: context did not become ready:" << state;
            Q_EMIT errorOccurred(u"PulseAudio: context not ready"_s);
            return false;
        }
    }

    m_pa->context = ctx.get();
    ctx.release();
    m_pa->contextReady = true;
    cleanupGuard.dismiss();

    qCDebug(dragonMultimediaAudio) << "PulseAudio connected";
    return true;
}

void DragonPulseAudioSink::disconnectFromServer()
{
    m_pa->contextReady = false;

    if (m_pa->mainloop) {
        ScopedMainloopLock lock(m_pa->mainloop);

        if (m_pa->stream) {
            pa_stream_set_state_callback(m_pa->stream, nullptr, nullptr);
            pa_stream_set_write_callback(m_pa->stream, nullptr, nullptr);
            pa_stream_set_underflow_callback(m_pa->stream, nullptr, nullptr);
            pa_stream_disconnect(m_pa->stream);
            pa_stream_unref(m_pa->stream);
            m_pa->stream = nullptr;
        }

        if (m_pa->context) {
            pa_context_disconnect(m_pa->context);
            pa_context_unref(m_pa->context);
            m_pa->context = nullptr;
        }
    }

    if (m_pa->mainloop) {
        pa_threaded_mainloop_stop(m_pa->mainloop);
        pa_threaded_mainloop_free(m_pa->mainloop);
        m_pa->mainloop = nullptr;
    }
}

void DragonPulseAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMultimediaAudio) << "PulseAudio open" << sampleRate << channels;

    if (m_open.load(std::memory_order_acquire)) {
        qCDebug(dragonMultimediaAudio) << "open() called while already open closing old session";
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

    ScopedMainloopLock lock(m_pa->mainloop);

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.rate = static_cast<uint32_t>(sampleRate);
    ss.channels = static_cast<uint8_t>(channels);

    pa_channel_map channelMap;
    if (!pa_channel_map_init_auto(&channelMap, channels, PA_CHANNEL_MAP_DEFAULT)) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: failed to init channel map for" << channels << "channels";
        Q_EMIT errorOccurred(u"PulseAudio: failed to init channel map"_s);
        disconnectFromServer();
        return;
    }

    const char *streamName = m_streamName.empty() ? "DragonMultimedia" : m_streamName.c_str();
    m_pa->stream = pa_stream_new(m_pa->context, streamName, &ss, &channelMap);
    if (!m_pa->stream) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_stream_new failed:" << pa_strerror(pa_context_errno(m_pa->context));
        Q_EMIT errorOccurred(u"PulseAudio: failed to create stream"_s);
        disconnectFromServer();
        return;
    }

    auto streamGuard = qScopeGuard([this]() {
        pa_stream_set_state_callback(m_pa->stream, nullptr, nullptr);
        pa_stream_set_write_callback(m_pa->stream, nullptr, nullptr);
        pa_stream_set_underflow_callback(m_pa->stream, nullptr, nullptr);
        pa_stream_disconnect(m_pa->stream);
        pa_stream_unref(m_pa->stream);
        m_pa->stream = nullptr;
    });

    pa_stream_set_state_callback(m_pa->stream, DragonPulseAudioSink::streamStateCallback, this);
    pa_stream_set_write_callback(m_pa->stream, DragonPulseAudioSink::writeCallback, this);
    pa_stream_set_underflow_callback(m_pa->stream, DragonPulseAudioSink::underflowCallback, this);

    pa_buffer_attr bufferAttr;
    bufferAttr.maxlength = static_cast<uint32_t>(-1);
    bufferAttr.tlength = static_cast<uint32_t>(pa_bytes_per_second(&ss) * kDefaultBufferMs / 1000 * kDefaultTlengthMultiplier);
    bufferAttr.prebuf = 0;
    bufferAttr.minreq = sizeof(float) * 1024;
    bufferAttr.fragsize = sizeof(float) * 1024;

    pa_cvolume cvol;
    pa_cvolume_set(&cvol, channels, pa_sw_volume_from_linear(m_cachedGain.load(std::memory_order_relaxed)));

    int ret = pa_stream_connect_playback(m_pa->stream,
                                         nullptr,
                                         &bufferAttr,
                                         static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING),
                                         &cvol,
                                         nullptr);

    if (ret < 0) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: pa_stream_connect_playback failed:" << pa_strerror(pa_context_errno(m_pa->context));
        Q_EMIT errorOccurred(u"PulseAudio: failed to connect stream"_s);
        disconnectFromServer();
        return;
    }

    pa_stream_state_t streamState = pa_stream_get_state(m_pa->stream);
    while (PA_STREAM_IS_GOOD(streamState) && streamState != PA_STREAM_READY) {
        pa_threaded_mainloop_wait(m_pa->mainloop);
        streamState = pa_stream_get_state(m_pa->stream);
    }

    if (!PA_STREAM_IS_GOOD(streamState) || streamState != PA_STREAM_READY) {
        qCCritical(dragonMultimediaAudio) << "PulseAudio: stream did not become ready";
        Q_EMIT errorOccurred(u"PulseAudio: stream not ready"_s);
        disconnectFromServer();
        return;
    }

    m_pa->sinkInputIndex = pa_stream_get_index(m_pa->stream);

    streamGuard.dismiss();

    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    qCDebug(dragonMultimediaAudio) << "PulseAudio stream opened, sink_input_index:" << m_pa->sinkInputIndex;
}

void DragonPulseAudioSink::close()
{
    qCDebug(dragonMultimediaAudio) << "PulseAudio close()";

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

    qCDebug(dragonMultimediaAudio) << "PulseAudio close() complete";
}

void DragonPulseAudioSink::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop);
        pa_operation *op = pa_stream_cork(m_pa->stream, 1, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void DragonPulseAudioSink::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop);
        pa_operation *op = pa_stream_cork(m_pa->stream, 0, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

void DragonPulseAudioSink::setGain(float linearGain)
{
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    if (m_pa->stream && m_pa->mainloop && m_open.load(std::memory_order_acquire)) {
        ScopedMainloopLock lock(m_pa->mainloop);
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
    pa_operation *op = pa_context_set_sink_input_volume(m_pa->context, m_pa->sinkInputIndex, &cvol, nullptr, nullptr);
    if (op) {
        pa_operation_unref(op);
    }
}

void DragonPulseAudioSink::clearStream()
{
    if (m_pa->mainloop && m_pa->stream) {
        ScopedMainloopLock lock(m_pa->mainloop);
        pa_operation *op = pa_stream_flush(m_pa->stream, nullptr, nullptr);
        if (op) {
            pa_operation_unref(op);
        }
    }
}

int64_t DragonPulseAudioSink::deviceQueuedSamples() const
{
    if (!m_pa->mainloop || !m_pa->stream || !m_open.load(std::memory_order_acquire)) {
        return 0;
    }

    ScopedMainloopLock lock(m_pa->mainloop);

    const pa_timing_info *ti = pa_stream_get_timing_info(m_pa->stream);
    if (!ti) {
        return 0;
    }

    const int64_t bytesQueued = ti->write_index - ti->read_index;
    if (bytesQueued < 0) {
        return 0;
    }

    return bytesQueued / static_cast<int64_t>(sizeof(float));
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

    ScopedMainloopLock lock(m_pa->mainloop);

    const pa_timing_info *ti = pa_stream_get_timing_info(m_pa->stream);
    if (!ti) {
        return -1;
    }

    const int64_t bytesQueued = ti->write_index - ti->read_index;
    if (bytesQueued < 0) {
        return 0;
    }

    return static_cast<int>(bytesQueued / static_cast<int64_t>(channels * sizeof(float)));
}

int DragonPulseAudioSink::audioBufferUs() const
{
    const int frames = audioBufferFrames();
    if (frames <= 0 || currentSampleRate() <= 0) {
        return -1;
    }

    return static_cast<int>((static_cast<int64_t>(frames) * 1000000) / currentSampleRate());
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
    m_streamName = name.toStdString();
}

void DragonPulseAudioSink::drainCallback(pa_stream *s, int success, void *userdata)
{
    Q_UNUSED(s);
    auto *self = static_cast<DragonPulseAudioSink *>(userdata);
    if (!success) {
        qCWarning(dragonMultimediaAudio) << "PulseAudio stream drain failed";
    }
    Q_EMIT self->drained();
}

void DragonPulseAudioSink::underflowCallback(pa_stream *s, void *userdata)
{
    Q_UNUSED(s);
    Q_UNUSED(userdata);
    qCDebug(dragonMultimediaAudio) << "PulseAudio underflow detected";
}

void DragonPulseAudioSink::resetDrainState()
{
    m_drainRequested.store(false, std::memory_order_release);
    DragonAudioSink::resetDrainState();
}

#include "dragonpulseaudiosink.moc"
