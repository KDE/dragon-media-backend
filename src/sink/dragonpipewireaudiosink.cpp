/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonpipewireaudiosink.h"

#include "dragonmediabackend_audio_logging.h"

#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(DragonPipeWireAudioSink, "pipewire_sink.json")

#include <QGuiApplication>
#include <QScopeGuard>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>

#include "dragonthreadname.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <vector>

using namespace Qt::StringLiterals;

static constexpr int PW_POD_BUFFER_LENGTH = 1024;
static constexpr uint32_t kDefaultQuantumFrames = 1024;

struct PwThreadLoopDeleter {
    void operator()(pw_thread_loop *loop) const noexcept
    {
        if (loop) {
            pw_thread_loop_stop(loop);
            pw_thread_loop_destroy(loop);
        }
    }
};

struct PwStreamDeleter {
    void operator()(pw_stream *stream) const noexcept
    {
        if (stream) {
            pw_stream_destroy(stream);
        }
    }
};

struct PwPropertiesDeleter {
    void operator()(pw_properties *props) const noexcept
    {
        if (props) {
            pw_properties_free(props);
        }
    }
};

struct PwLoopDeleter {
    void operator()(pw_loop *loop) const noexcept
    {
        if (loop) {
            pw_loop_destroy(loop);
        }
    }
};

struct PwContextDeleter {
    void operator()(pw_context *ctx) const noexcept
    {
        if (ctx) {
            pw_context_destroy(ctx);
        }
    }
};

struct PwCoreDisconnector {
    void operator()(pw_core *core) const noexcept
    {
        if (core) {
            pw_core_disconnect(core);
        }
    }
};

using PwThreadLoopPtr = std::unique_ptr<pw_thread_loop, PwThreadLoopDeleter>;
using PwStreamPtr = std::unique_ptr<pw_stream, PwStreamDeleter>;
using PwPropertiesPtr = std::unique_ptr<pw_properties, PwPropertiesDeleter>;
using PwLoopPtr = std::unique_ptr<pw_loop, PwLoopDeleter>;
using PwContextPtr = std::unique_ptr<pw_context, PwContextDeleter>;
using PwCorePtr = std::unique_ptr<pw_core, PwCoreDisconnector>;

class PwThreadLoopLock
{
public:
    explicit PwThreadLoopLock(pw_thread_loop *loop)
        : m_loop(loop)
    {
        pw_thread_loop_lock(m_loop);
    }
    ~PwThreadLoopLock()
    {
        pw_thread_loop_unlock(m_loop);
    }
    PwThreadLoopLock(const PwThreadLoopLock &) = delete;
    PwThreadLoopLock &operator=(const PwThreadLoopLock &) = delete;

private:
    pw_thread_loop *m_loop;
};

struct DragonPipeWireAudioSink::PwState {
    PwThreadLoopPtr loop;
    PwStreamPtr stream;
    spa_hook streamListener{};
};

static constexpr pw_stream_events s_streamEvents = [] {
    struct pw_stream_events ev{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.process = DragonPipeWireAudioSink::onProcess;
    ev.control_info = DragonPipeWireAudioSink::onControlInfo;
    ev.param_changed = DragonPipeWireAudioSink::onParamChanged;
    ev.drained = DragonPipeWireAudioSink::onDrained;
    return ev;
}();

DragonPipeWireAudioSink::DragonPipeWireAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
    , m_pw(std::make_unique<PwState>())
{
    Q_UNUSED(args);
    pw_init(nullptr, nullptr);
    qCDebug(dragonMediaBackendAudio) << "PipeWire audio sink created, library version:" << pw_get_library_version();
}

DragonPipeWireAudioSink::~DragonPipeWireAudioSink()
{
    DragonPipeWireAudioSink::close();
    pw_deinit();
}

bool DragonPipeWireAudioSink::probe()
{
    qCDebug(dragonMediaBackendAudio) << "PipeWire probe() checking daemon connectivity";

    PwLoopPtr loop(pw_loop_new(nullptr));
    if (!loop) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire probe() pw_loop_new failed";
        return false;
    }

    PwContextPtr context(pw_context_new(loop.get(), nullptr, 0));
    if (!context) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire probe() pw_context_new failed";
        return false;
    }

    PwCorePtr core(pw_context_connect(context.get(), nullptr, 0));
    const bool alive = static_cast<bool>(core);

    if (alive) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire probe() daemon reachable, connection confirmed";
    } else {
        qCDebug(dragonMediaBackendAudio) << "PipeWire probe() daemon unreachable, errno =" << errno;
    }

    return alive;
}

void DragonPipeWireAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendAudio) << "PipeWire open" << sampleRate << channels;

    if (m_open.load(std::memory_order_acquire)) {
        qCDebug(dragonMediaBackendAudio) << "open() called while already open closing old session";
        close();
    }

    setFormat(sampleRate, channels);
    reset();

    preAllocateCallbackBuffer(kDefaultQuantumFrames * static_cast<size_t>(channels));

    PwThreadLoopPtr loop(pw_thread_loop_new("dragon-pw", nullptr));
    if (!loop) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to create thread loop";
        Q_EMIT errorOccurred(i18n("PipeWire: failed to create thread loop"));
        return;
    }

    PwPropertiesPtr props(pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr));

    const QString appName = QGuiApplication::applicationDisplayName();
    const QByteArray appNameUtf8 = appName.toUtf8();
    const char *nodeName = appNameUtf8.isEmpty() ? "DragonMediaBackend" : appNameUtf8.constData();

    pw_properties_set(props.get(), PW_KEY_APP_NAME, nodeName);
    pw_properties_set(props.get(), PW_KEY_NODE_NAME, nodeName);
    pw_properties_set(props.get(), PW_KEY_NODE_DESCRIPTION, nodeName);

    const QString mediaName = resolvedStreamName();
    const QByteArray mediaNameUtf8 = mediaName.toUtf8();
    pw_properties_set(props.get(), PW_KEY_MEDIA_NAME, mediaNameUtf8.constData());

    pw_properties_set(props.get(), PW_KEY_NODE_ALWAYS_PROCESS, "true");

    const QString appIconName = applicationIconName();
    if (!appIconName.isEmpty()) {
        const QByteArray appIconNameUtf8 = appIconName.toUtf8();
        pw_properties_set(props.get(), PW_KEY_APP_ICON_NAME, appIconNameUtf8.constData());
        pw_properties_set(props.get(), PW_KEY_APP_ID, appIconNameUtf8.constData());
    }

    const QByteArray testSinkName = qgetenv("DRAGON_PW_TEST_SINK_NAME");
    if (!testSinkName.isEmpty()) {
        pw_properties_set(props.get(), PW_KEY_TARGET_OBJECT, testSinkName.constData());
        qCDebug(dragonMediaBackendAudio) << "PipeWire: overriding target to" << testSinkName;
    }

    // pw_stream_new_simple takes ownership of props release the unique_ptr so it doesn't double-free.
    PwStreamPtr stream(pw_stream_new_simple(pw_thread_loop_get_loop(loop.get()), nodeName, props.release(), &s_streamEvents, this));

    if (!stream) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to create stream";
        Q_EMIT errorOccurred(i18n("PipeWire: failed to create stream"));
        return;
    }

    uint8_t podBuffer[PW_POD_BUFFER_LENGTH];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

    spa_audio_info_raw audioInfo =
        SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = static_cast<uint32_t>(sampleRate), .channels = static_cast<uint32_t>(channels));

    // Explicit channel positions for mono and stereo.
    // For >2 channels, PipeWire infers positions from the channel count.
    if (channels == 1) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels == 2) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
        audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    const spa_pod *params = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audioInfo);
    if (!params) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to build audio format pod";
        Q_EMIT errorOccurred(i18n("PipeWire: failed to build audio format"));
        return;
    }

    constexpr auto streamFlags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

    int res = pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, &params, 1);
    if (res != 0) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to connect stream:" << res;
        Q_EMIT errorOccurred(i18n("PipeWire: failed to connect stream"));
        return;
    }

    res = pw_thread_loop_start(loop.get());
    if (res != 0) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to start thread loop:" << res;
        Q_EMIT errorOccurred(i18n("PipeWire: failed to start thread loop"));
        return;
    }

    // Set initial channel volumes under the loop lock
    {
        const auto ch = static_cast<uint32_t>(currentChannels());
        m_volumesScratch.assign(ch, m_cachedGain.load(std::memory_order_relaxed));
        PwThreadLoopLock lock(loop.get());
        pw_stream_set_control(stream.get(), SPA_PROP_channelVolumes, ch, m_volumesScratch.data(), 0);
    }

    // Success transfer ownership to m_pw
    m_pw->loop = std::move(loop);
    m_pw->stream = std::move(stream);

    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    qCDebug(dragonMediaBackendAudio) << "PipeWire audio device opened";
}

void DragonPipeWireAudioSink::close()
{
    qCDebug(dragonMediaBackendAudio) << "PipeWire close()";

    m_open.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    // pw_stream_destroy must be called under the thread loop lock.
    if (m_pw->loop && m_pw->stream) {
        PwThreadLoopLock lock(m_pw->loop.get());
        m_pw->stream.reset();
    }

    m_pw->loop.reset();

    // Wait for in-flight callbacks to finish before fully returning.
    {
        std::unique_lock lock(m_callbackDoneMutex);
        m_callbackDoneCv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return m_activeCallbacks.load(std::memory_order_acquire) == 0;
        });
    }

    qCDebug(dragonMediaBackendAudio) << "PipeWire close() complete";
}

void DragonPipeWireAudioSink::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        PwThreadLoopLock lock(m_pw->loop.get());
        pw_stream_set_active(m_pw->stream.get(), false);
    }
}

void DragonPipeWireAudioSink::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        PwThreadLoopLock lock(m_pw->loop.get());
        pw_stream_set_active(m_pw->stream.get(), true);
    }
}

void DragonPipeWireAudioSink::setGain(float linearGain)
{
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    if (m_pw->stream && m_open.load(std::memory_order_acquire)) {
        PwThreadLoopLock lock(m_pw->loop.get());
        setChannelVolumes(linearGain);
    }
}

void DragonPipeWireAudioSink::setMuted(bool muted)
{
    m_cachedMuted.store(muted, std::memory_order_relaxed);

    if (m_pw->stream && m_open.load(std::memory_order_acquire)) {
        float muteVal = muted ? 1.0f : 0.0f;
        PwThreadLoopLock lock(m_pw->loop.get());
        pw_stream_set_control(m_pw->stream.get(), SPA_PROP_mute, 1, &muteVal, 0);
    }

    DragonAudioSink::setMuted(muted);
}

void DragonPipeWireAudioSink::setStreamName(const QString &name)
{
    DragonAudioSink::setStreamName(name);

    if (!m_pw->stream || !m_open.load(std::memory_order_acquire)) {
        return;
    }

    const QString mediaName = resolvedStreamName();
    const QByteArray mediaNameUtf8 = mediaName.toUtf8();

    const spa_dict_item items[] = {
        {PW_KEY_MEDIA_NAME, mediaNameUtf8.constData()},
    };
    const spa_dict dict = SPA_DICT_INIT_ARRAY(items);

    PwThreadLoopLock lock(m_pw->loop.get());
    pw_stream_update_properties(m_pw->stream.get(), &dict);
    qCDebug(dragonMediaBackendAudio) << "PipeWire: setStreamName() updated media.name to:" << mediaName;
}

void DragonPipeWireAudioSink::clearStream()
{
    if (m_pw->stream) {
        pw_stream_flush(m_pw->stream.get(), false);
    }
}

void DragonPipeWireAudioSink::setChannelVolumes(float linearGain)
{
    const auto ch = static_cast<uint32_t>(currentChannels());
    m_volumesScratch.assign(ch, linearGain);
    pw_stream_set_control(m_pw->stream.get(), SPA_PROP_channelVolumes, ch, m_volumesScratch.data(), 0);
}

qint64 DragonPipeWireAudioSink::deviceQueuedSamples() const
{
    if (!m_pw->stream) {
        return 0;
    }

    pw_time time{};
    if (pw_stream_get_time_n(m_pw->stream.get(), &time, sizeof(time)) != 0) {
        return 0;
    }

    return static_cast<qint64>(time.queued) / sizeof(float);
}

int DragonPipeWireAudioSink::audioBufferFrames() const
{
    if (!m_pw->stream) {
        return -1;
    }

    pw_time time{};
    if (pw_stream_get_time_n(m_pw->stream.get(), &time, sizeof(time)) != 0) {
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
    return static_cast<int>((static_cast<qint64>(frames) * 1000000) / currentSampleRate());
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
        DragonThreadName::set("dragon-pw-cb");
        audioThreadNamed = true;
    }

    auto *stream = self->m_pw->stream.get();
    if (!stream) {
        return;
    }

    pw_buffer *pwBuf = pw_stream_dequeue_buffer(stream);
    if (!pwBuf) {
        return;
    }

    spa_buffer *spaBuf = pwBuf->buffer;
    if (!spaBuf || spaBuf->n_datas < 1 || !spaBuf->datas[0].data) {
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    const uint32_t maxBytes = spaBuf->datas[0].maxsize;
    const uint32_t maxFramesFromSize = maxBytes / static_cast<uint32_t>(self->currentChannels() * sizeof(float));

    uint32_t requestedFrames = pwBuf->requested;
    if (requestedFrames == 0) {
        pw_time pwt{};
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

    pw_time pwt{};
    qint64 latencyUs = 0;
    if (pw_stream_get_time_n(stream, &pwt, sizeof(pwt)) == 0 && pwt.rate.denom > 0) {
        latencyUs = (pwt.delay * 1'000'000LL * pwt.rate.num) / pwt.rate.denom;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(latencyUs);

    auto pcm = self->processAudioCallback(maxSamples, pts);

    auto *dst = static_cast<float *>(spaBuf->datas[0].data);

    if (pcm.empty() && self->m_decodeFinished.load(std::memory_order_acquire) && !self->m_drainInitiated.exchange(true, std::memory_order_acq_rel)
        && self->m_decodeFinished.load(std::memory_order_acquire)) {
        spaBuf->datas[0].chunk->size = 0;
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
        pwBuf->size = 0;
        pw_stream_queue_buffer(stream, pwBuf);
        pw_stream_flush(stream, true);
        return;
    }

    if (pcm.empty()) {
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
    } else {
        spaBuf->datas[0].chunk->flags = 0;
    }

    if (pcm.size() < maxSamples) {
        std::ranges::fill(std::span{dst + pcm.size(), maxSamples - pcm.size()}, 0.0f);
    }

    if (!pcm.empty()) {
        std::ranges::copy(pcm, dst);
    }

    spaBuf->datas[0].chunk->offset = 0;
    spaBuf->datas[0].chunk->stride = static_cast<int32_t>(channels * sizeof(float));
    spaBuf->datas[0].chunk->size = static_cast<uint32_t>(maxSamples * sizeof(float));

    pwBuf->size = spaBuf->datas[0].chunk->size;

    pw_stream_queue_buffer(stream, pwBuf);
}

void DragonPipeWireAudioSink::onControlInfo(void *userdata, uint32_t id, const pw_stream_control *control)
{
    auto *self = static_cast<DragonPipeWireAudioSink *>(userdata);
    if (!self || !control || control->n_values == 0) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PipeWire control_info id:" << id;

    if (id == SPA_PROP_channelVolumes) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < control->n_values; ++i) {
            sum += control->values[i];
        }
        const float avgGain = sum / static_cast<float>(control->n_values);

        float cached = self->m_cachedGain.load(std::memory_order_relaxed);
        if (qAbs(avgGain - cached) < 0.001f) {
            return;
        }
        self->m_cachedGain.store(avgGain, std::memory_order_relaxed);

        qCDebug(dragonMediaBackendAudio) << "PipeWire external volume change via control_info, avgGain:" << avgGain;
        QMetaObject::invokeMethod(
            self,
            [self, avgGain]() {
                self->onExternalVolumeChanged(avgGain);
            },
            Qt::QueuedConnection);
    } else if (id == SPA_PROP_mute) {
        const bool muted = control->values[0] >= 0.5f;
        if (muted == self->m_cachedMuted.load(std::memory_order_relaxed)) {
            return;
        }
        self->m_cachedMuted.store(muted, std::memory_order_relaxed);

        qCDebug(dragonMediaBackendAudio) << "PipeWire external mute change via control_info, muted:" << muted;
        QMetaObject::invokeMethod(
            self,
            [self, muted]() {
                self->setMuted(muted);
            },
            Qt::QueuedConnection);
    }
}

void DragonPipeWireAudioSink::onParamChanged(void *userdata, uint32_t id, const spa_pod *param)
{
    auto *self = static_cast<DragonPipeWireAudioSink *>(userdata);
    if (!self || !param) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PipeWire param_changed id:" << id;

    if (id != SPA_PARAM_Props) {
        return;
    }

    float volume = 0.0f;
    if (const spa_pod_prop *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_volume); prop != nullptr && spa_pod_get_float(&prop->value, &volume) == 0) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire param_changed SPA_PROP_volume:" << volume;
        QMetaObject::invokeMethod(
            self,
            [self, volume]() {
                self->onExternalVolumeChanged(volume);
            },
            Qt::QueuedConnection);
    }

    bool muted = false;
    if (const spa_pod_prop *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute); prop != nullptr && spa_pod_get_bool(&prop->value, &muted) == 0) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire param_changed SPA_PROP_mute:" << muted;
        QMetaObject::invokeMethod(
            self,
            [self, muted]() {
                self->setMuted(muted);
            },
            Qt::QueuedConnection);
    }
}

void DragonPipeWireAudioSink::onDrained(void *userdata)
{
    Q_EMIT static_cast<DragonPipeWireAudioSink *>(userdata)->drained();
}

void DragonPipeWireAudioSink::resetDrainState()
{
    DragonAudioSink::resetDrainState();
    m_drainInitiated.store(false, std::memory_order_release);
}

#include "dragonpipewireaudiosink.moc"
