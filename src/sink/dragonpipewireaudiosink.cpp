/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonpipewireaudiosink.h"

#include "dragonmultimedia_audio_logging.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(DragonPipeWireAudioSink, "pipewire_sink.json")

#include <QGuiApplication>
#include <QScopeGuard>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <pthread.h>
#include <span>
#include <vector>

using namespace Qt::StringLiterals;

static constexpr int PW_POD_BUFFER_LENGTH = 1024;
static constexpr uint32_t kDefaultQuantumFrames = 1024;

struct DragonPipeWireAudioSink::PwState {
    pw_thread_loop *loop = nullptr;
    pw_stream *stream = nullptr;
    spa_hook streamListener{};
};

static const struct pw_stream_events s_streamEvents = [] {
    struct pw_stream_events ev{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.process = DragonPipeWireAudioSink::onProcess;
    ev.control_info = DragonPipeWireAudioSink::onControlInfo;
    ev.param_changed = DragonPipeWireAudioSink::onParamChanged;
    return ev;
}();

DragonPipeWireAudioSink::DragonPipeWireAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
    , m_pw(std::make_unique<PwState>())
{
    Q_UNUSED(args);
    pw_init(nullptr, nullptr);
    qCDebug(dragonMultimediaAudio) << "PipeWire audio sink created, library version:" << pw_get_library_version();
}

DragonPipeWireAudioSink::~DragonPipeWireAudioSink()
{
    DragonPipeWireAudioSink::close();
    pw_deinit();
}

bool DragonPipeWireAudioSink::probe()
{
    qCDebug(dragonMultimediaAudio) << "PipeWire probe() checking daemon connectivity";

    auto *loop = pw_loop_new(nullptr);
    if (!loop) {
        qCDebug(dragonMultimediaAudio) << "PipeWire probe() pw_loop_new failed";
        return false;
    }

    struct pw_context *context = pw_context_new(loop, nullptr, 0);
    if (!context) {
        qCDebug(dragonMultimediaAudio) << "PipeWire probe() pw_context_new failed";
        pw_loop_destroy(loop);
        return false;
    }

    struct pw_core *core = pw_context_connect(context, nullptr, 0);
    const bool alive = (core != nullptr);

    if (alive) {
        qCDebug(dragonMultimediaAudio) << "PipeWire probe() daemon reachable, connection confirmed";
        pw_core_disconnect(core);
    } else {
        qCDebug(dragonMultimediaAudio) << "PipeWire probe() daemon unreachable, errno =" << errno;
    }

    pw_context_destroy(context);
    pw_loop_destroy(loop);

    return alive;
}

void DragonPipeWireAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMultimediaAudio) << "PipeWire open" << sampleRate << channels;

    if (m_open.load(std::memory_order_acquire)) {
        qCDebug(dragonMultimediaAudio) << "open() called while already open closing old session";
        close();
    }

    setFormat(sampleRate, channels);
    reset();

    struct PwOpenGuard {
        struct PwState &pw;
        bool dismissed = false;

        ~PwOpenGuard()
        {
            if (dismissed)
                return;
            if (pw.stream) {
                pw_stream_destroy(pw.stream);
                pw.stream = nullptr;
            }
            if (pw.loop) {
                pw_thread_loop_destroy(pw.loop);
                pw.loop = nullptr;
            }
        }
    };

    PwOpenGuard guard{*m_pw};

    m_pw->loop = pw_thread_loop_new("dragon-pw", nullptr);
    if (!m_pw->loop) {
        qCCritical(dragonMultimediaAudio) << "PipeWire: failed to create thread loop";
        Q_EMIT errorOccurred(u"PipeWire: failed to create thread loop"_s);
        return;
    }

    auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr);

    const QString appName = QGuiApplication::applicationDisplayName();
    const QByteArray appNameUtf8 = appName.toUtf8();
    const char *nodeName = appNameUtf8.isEmpty() ? "DragonMultimedia" : appNameUtf8.constData();

    pw_properties_set(props, PW_KEY_APP_NAME, nodeName);
    pw_properties_set(props, PW_KEY_NODE_NAME, nodeName);
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, nodeName);
    pw_properties_set(props, PW_KEY_MEDIA_NAME, nodeName);

    pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

    const QByteArray testSinkName = qgetenv("DRAGON_PW_TEST_SINK_NAME");
    if (!testSinkName.isEmpty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, testSinkName.constData());
        qCDebug(dragonMultimediaAudio) << "PipeWire: overriding target to" << testSinkName;
    }

    m_pw->stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_pw->loop), nodeName, props, &s_streamEvents, this);

    if (!m_pw->stream) {
        qCCritical(dragonMultimediaAudio) << "PipeWire: failed to create stream";
        Q_EMIT errorOccurred(u"PipeWire: failed to create stream"_s);
        return;
    }

    uint8_t podBuffer[PW_POD_BUFFER_LENGTH];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

    struct spa_audio_info_raw audioInfo = {};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    audioInfo.rate = static_cast<uint32_t>(sampleRate);
    audioInfo.channels = static_cast<uint32_t>(channels);

    // Explicit channel positions for mono and stereo.
    // For >2 channels, PipeWire infers positions from the channel count.
    if (channels == 1) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels == 2) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
        audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    const struct spa_pod *params = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audioInfo);
    if (!params) {
        qCCritical(dragonMultimediaAudio) << "PipeWire: failed to build audio format pod";
        Q_EMIT errorOccurred(u"PipeWire: failed to build audio format"_s);
        return;
    }

    constexpr auto streamFlags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

    int res = pw_stream_connect(m_pw->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, &params, 1);

    if (res != 0) {
        qCCritical(dragonMultimediaAudio) << "PipeWire: failed to connect stream:" << res;
        Q_EMIT errorOccurred(u"PipeWire: failed to connect stream"_s);
        return;
    }

    res = pw_thread_loop_start(m_pw->loop);
    if (res != 0) {
        qCCritical(dragonMultimediaAudio) << "PipeWire: failed to start thread loop:" << res;
        Q_EMIT errorOccurred(u"PipeWire: failed to start thread loop"_s);
        return;
    }

    pw_thread_loop_lock(m_pw->loop);
    setChannelVolumes(m_cachedGain);
    pw_thread_loop_unlock(m_pw->loop);

    guard.dismissed = true;
    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    qCDebug(dragonMultimediaAudio) << "PipeWire audio device opened";
}

void DragonPipeWireAudioSink::close()
{
    qCDebug(dragonMultimediaAudio) << "PipeWire close()";

    m_open.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        pw_stream_destroy(m_pw->stream);
        m_pw->stream = nullptr;
        pw_thread_loop_unlock(m_pw->loop);
    }

    if (auto *loop = std::exchange(m_pw->loop, nullptr)) {
        pw_thread_loop_stop(loop);
        pw_thread_loop_destroy(loop);
    }

    {
        std::unique_lock lock(m_callbackDoneMutex);
        m_callbackDoneCv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return m_activeCallbacks.load(std::memory_order_acquire) == 0;
        });
    }

    qCDebug(dragonMultimediaAudio) << "PipeWire close() complete";
}

void DragonPipeWireAudioSink::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        pw_stream_set_active(m_pw->stream, false);
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        pw_stream_set_active(m_pw->stream, true);
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::setGain(float linearGain)
{
    m_cachedGain = linearGain;

    if (m_pw->stream && m_open.load(std::memory_order_acquire)) {
        pw_thread_loop_lock(m_pw->loop);
        setChannelVolumes(linearGain);
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::clearStream()
{
    if (m_pw->stream) {
        pw_stream_flush(m_pw->stream, false);
    }
}

void DragonPipeWireAudioSink::setChannelVolumes(float linearGain)
{
    const uint32_t ch = static_cast<uint32_t>(currentChannels());
    m_volumesScratch.assign(ch, linearGain);
    pw_stream_set_control(m_pw->stream, SPA_PROP_channelVolumes, ch, m_volumesScratch.data());
}

int64_t DragonPipeWireAudioSink::deviceQueuedSamples() const
{
    if (!m_pw->stream) {
        return 0;
    }

    struct pw_time time{};
    if (pw_stream_get_time_n(m_pw->stream, &time, sizeof(time)) != 0) {
        return 0;
    }

    return static_cast<int64_t>(time.queued) / sizeof(float);
}

int DragonPipeWireAudioSink::audioBufferFrames() const
{
    if (!m_pw->stream) {
        return -1;
    }

    struct pw_time time{};
    if (pw_stream_get_time_n(m_pw->stream, &time, sizeof(time)) != 0) {
        return -1;
    }

    return static_cast<int>(time.queued / (currentChannels() * sizeof(float)));
}

int DragonPipeWireAudioSink::audioBufferUs() const
{
    int frames = audioBufferFrames();
    if (frames < 0 || currentSampleRate() == 0) {
        return -1;
    }
    return static_cast<int>((static_cast<int64_t>(frames) * 1000000) / currentSampleRate());
}

bool DragonPipeWireAudioSink::isDeviceOpen() const
{
    return m_open.load(std::memory_order_acquire);
}

bool DragonPipeWireAudioSink::isPaused() const
{
    return m_paused.load(std::memory_order_acquire);
}

void DragonPipeWireAudioSink::onProcess(void *userdata)
{
    auto *self = static_cast<DragonPipeWireAudioSink *>(userdata);
    if (!self || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    self->m_activeCallbacks.fetch_add(1, std::memory_order_relaxed);
    auto guard = qScopeGuard([self]() noexcept {
        self->m_activeCallbacks.fetch_sub(1, std::memory_order_relaxed);
        self->m_callbackDoneCv.notify_one();
    });

    thread_local bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        pthread_setname_np(pthread_self(), "dragon-pw-cb");
        audioThreadNamed = true;
    }

    auto *stream = self->m_pw->stream;
    if (!stream) {
        return;
    }

    struct pw_buffer *pwBuf = pw_stream_dequeue_buffer(stream);
    if (!pwBuf) {
        return;
    }

    struct spa_buffer *spaBuf = pwBuf->buffer;
    if (!spaBuf || spaBuf->n_datas < 1 || !spaBuf->datas[0].data) {
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    const uint32_t maxBytes = spaBuf->datas[0].maxsize;
    const uint32_t maxFramesFromSize = maxBytes / static_cast<uint32_t>(self->currentChannels() * sizeof(float));

    uint32_t requestedFrames = pwBuf->requested;
    if (requestedFrames == 0) {
        struct pw_time pwt{};
        if (pw_stream_get_time_n(stream, &pwt, sizeof(pwt)) == 0 && pwt.size > 0) {
            requestedFrames = pwt.size;
        } else {
            requestedFrames = kDefaultQuantumFrames;
        }
    }
    requestedFrames = std::min(requestedFrames, maxFramesFromSize);

    if (requestedFrames == 0) {
        spaBuf->datas[0].chunk->size = 0;
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
        pwBuf->size = 0;
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    const size_t maxSamples = requestedFrames * self->currentChannels();

    if (maxSamples == 0) {
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    const int channels = self->currentChannels();

    struct pw_time pwt{};
    int64_t latencyUs = 0;
    if (pw_stream_get_time_n(stream, &pwt, sizeof(pwt)) == 0 && pwt.rate.denom > 0) {
        latencyUs = (pwt.delay * 1'000'000LL * pwt.rate.num) / pwt.rate.denom;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(latencyUs);

    auto pcm = self->processAudioCallback(maxSamples, pts);

    auto *dst = static_cast<float *>(spaBuf->datas[0].data);

    if (pcm.size() < maxSamples) {
        std::ranges::fill(std::span{dst + pcm.size(), maxSamples - pcm.size()}, 0.0f);
    }

    if (!pcm.empty()) {
        std::ranges::copy(pcm, dst);
        spaBuf->datas[0].chunk->flags = 0;
    } else {
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
    }

    spaBuf->datas[0].chunk->offset = 0;
    spaBuf->datas[0].chunk->stride = static_cast<int32_t>(channels * sizeof(float));
    spaBuf->datas[0].chunk->size = static_cast<uint32_t>(maxSamples * sizeof(float));

    pwBuf->size = spaBuf->datas[0].chunk->size;

    pw_stream_queue_buffer(stream, pwBuf);
}

void DragonPipeWireAudioSink::onControlInfo(void *userdata, uint32_t id, const struct pw_stream_control *control)
{
    auto *self = static_cast<DragonPipeWireAudioSink *>(userdata);
    if (!self || !control || control->n_values == 0) {
        return;
    }

    qCDebug(dragonMultimediaAudio) << "PipeWire control_info id:" << id;

    if (id == SPA_PROP_channelVolumes) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < control->n_values; ++i) {
            sum += control->values[i];
        }
        const float avgGain = sum / static_cast<float>(control->n_values);

        if (qAbs(avgGain - self->m_cachedGain) < 0.001f) {
            return;
        }
        self->m_cachedGain = avgGain;

        qCDebug(dragonMultimediaAudio) << "PipeWire external volume change via control_info, avgGain:" << avgGain;
        QMetaObject::invokeMethod(
            self,
            [self, avgGain]() {
                self->onExternalVolumeChanged(avgGain);
            },
            Qt::QueuedConnection);
    }
}

void DragonPipeWireAudioSink::onParamChanged(void *userdata, uint32_t id, const struct spa_pod *param)
{
    auto *self = static_cast<DragonPipeWireAudioSink *>(userdata);
    if (!self || !param) {
        return;
    }

    qCDebug(dragonMultimediaAudio) << "PipeWire param_changed id:" << id;

    if (id == SPA_PARAM_Props) {
        const struct spa_pod_prop *prop = nullptr;
        SPA_POD_OBJECT_FOREACH((const struct spa_pod_object *)param, prop)
        {
            if (prop->key == SPA_PROP_volume) {
                const float *val = reinterpret_cast<const float *>(SPA_POD_BODY(&prop->value));
                float value = val ? *val : 1.0f;
                qCDebug(dragonMultimediaAudio) << "PipeWire param_changed SPA_PROP_volume:" << value;
                QMetaObject::invokeMethod(
                    self,
                    [self, value]() {
                        self->onExternalVolumeChanged(value);
                    },
                    Qt::QueuedConnection);
                return;
            }
        }
    }
}

#include "dragonpipewireaudiosink.moc"
