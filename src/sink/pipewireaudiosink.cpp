/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/utils/dict.h>

#include "pipewireaudiosink.h"

#include "dragonmediabackend_audio_logging.h"

#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(PipeWireAudioSink, "pipewire_sink.json")

#include <QGuiApplication>
#include <QScopeGuard>

#include "threadname.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

static constexpr int PW_POD_BUFFER_LENGTH = 1024;
static constexpr uint32_t kDefaultQuantumFrames = 1024;

namespace
{

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

static constexpr pw_stream_events s_streamEvents = [] {
    struct pw_stream_events ev{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.process = PipeWireAudioSinkStream::onProcess;
    ev.state_changed = PipeWireAudioSinkStream::onStateChanged;
    ev.control_info = PipeWireAudioSinkStream::onControlInfo;
    ev.param_changed = PipeWireAudioSinkStream::onParamChanged;
    ev.drained = PipeWireAudioSinkStream::onDrained;
    return ev;
}();

} // namespace

struct PipeWireAudioSinkStream::PwStream {
    PwStreamPtr handle;
};

struct PipeWireAudioSink::PwState {
    PwThreadLoopPtr loop;
    PwContextPtr context;
    PwCorePtr core;

    std::vector<PipeWireAudioSinkStream *> streams;
};

PipeWireAudioSink::PipeWireAudioSink(QObject *parent, const QVariantList &args)
    : AudioSink(parent)
    , m_pw(std::make_unique<PwState>())
{
    Q_UNUSED(args);
    pw_init(nullptr, nullptr);
    qCDebug(dragonMediaBackendAudio) << "PipeWire audio device created, library version:" << pw_get_library_version();
}

PipeWireAudioSink::~PipeWireAudioSink()
{
    if (m_poisoned) {
        // A poisoned sink deliberately leaks its loop/context/core so a stuck
        // callback can never dereference destroyed PipeWire objects.
        std::ignore = m_pw.release();
        return;
    }
    disconnect();
    // No pw_deinit() here: other PipeWire users in this process (other
    // sinks, other libraries) must not have the global library state
    // pulled out from under them by one sink instance going away.
}

bool PipeWireAudioSink::probe()
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

std::size_t PipeWireAudioSink::preferredCallbackSamples(int sampleRate, int channels) const
{
    Q_UNUSED(sampleRate);
    constexpr std::size_t kMaxQuantumFrames = 8192;
    return std::max(static_cast<std::size_t>(kDefaultQuantumFrames), kMaxQuantumFrames) * static_cast<std::size_t>(std::max(1, channels));
}

bool PipeWireAudioSink::ensureConnected()
{
    if (m_poisoned) {
        return false;
    }
    if (m_pw->loop && m_pw->context && m_pw->core) {
        return true;
    }

    PwThreadLoopPtr loop(pw_thread_loop_new("dragon-pw", nullptr));
    if (!loop) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to create thread loop";
        return false;
    }

    PwContextPtr context(pw_context_new(pw_thread_loop_get_loop(loop.get()), nullptr, 0));
    if (!context) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to create context";
        return false;
    }

    int res = pw_thread_loop_start(loop.get());
    if (res != 0) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to start thread loop:" << res;
        return false;
    }

    PwCorePtr core;
    {
        PwThreadLoopLock lock(loop.get());
        core.reset(pw_context_connect(context.get(), nullptr, 0));
    }
    if (!core) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to connect core";
        return false;
    }

    m_pw->loop = std::move(loop);
    m_pw->context = std::move(context);
    m_pw->core = std::move(core);
    qCDebug(dragonMediaBackendAudio) << "PipeWire connection established";
    return true;
}

void PipeWireAudioSink::disconnect()
{
    if (!m_pw->loop) {
        return;
    }

    {
        PwThreadLoopLock lock(m_pw->loop.get());
        // Streams must have been closed (and unregistered) by their owning
        // voices before the connection goes away.
        Q_ASSERT(m_pw->streams.empty());
        m_pw->core.reset();
        m_pw->context.reset();
    }
    m_pw->loop.reset();
}

void PipeWireAudioSink::registerStream(PipeWireAudioSinkStream *stream)
{
    PwThreadLoopLock lock(m_pw->loop.get());
    m_pw->streams.push_back(stream);
}

void PipeWireAudioSink::unregisterStream(PipeWireAudioSinkStream *stream)
{
    if (!m_pw->loop) {
        return;
    }
    PwThreadLoopLock lock(m_pw->loop.get());
    std::erase(m_pw->streams, stream);
}

std::expected<std::unique_ptr<AudioSinkStream>, QString>
PipeWireAudioSink::createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit)
{
    qCDebug(dragonMediaBackendAudio) << "PipeWire createStream" << sampleRate << channels;

    if (!ensureConnected()) {
        return std::unexpected(i18n("PipeWire: connection to the PipeWire server failed"));
    }

    auto stream = std::make_unique<PipeWireAudioSinkStream>(*this, sampleRate, channels, renderer);
    if (!stream->connectToCore(volumeInit)) {
        return std::unexpected(i18n("PipeWire: failed to connect stream"));
    }
    registerStream(stream.get());
    return stream;
}

void PipeWireAudioSink::setStreamName(const QString &name)
{
    AudioSink::setStreamName(name);

    if (!m_pw->loop) {
        return;
    }

    const QString mediaName = resolvedStreamName();
    const QByteArray mediaNameUtf8 = mediaName.toUtf8();

    const spa_dict_item items[] = {
        {PW_KEY_MEDIA_NAME, mediaNameUtf8.constData()},
    };
    const spa_dict dict = SPA_DICT_INIT_ARRAY(items);

    PwThreadLoopLock lock(m_pw->loop.get());
    for (auto *stream : m_pw->streams) {
        if (auto *pwStream = stream->m_stream ? stream->m_stream->handle.get() : nullptr) {
            pw_stream_update_properties(pwStream, &dict);
        }
    }
    qCDebug(dragonMediaBackendAudio) << "PipeWire: setStreamName() updated media.name to:" << mediaName;
}

PipeWireAudioSinkStream::PipeWireAudioSinkStream(PipeWireAudioSink &sink, int sampleRate, int channels, AudioSinkStreamRenderer &renderer)
    : AudioSinkStream(renderer)
    , m_sink(sink)
    , m_sampleRate(sampleRate)
    , m_channels(channels)
    , m_stream(std::make_unique<PwStream>())
{
}

PipeWireAudioSinkStream::~PipeWireAudioSinkStream()
{
    std::ignore = close();
}

bool PipeWireAudioSinkStream::connectToCore(StreamVolumeInit volumeInit)
{
    m_firstVolumeReportSeen.store(false, std::memory_order_relaxed);
    m_gainSetByPipeline.store(volumeInit.linearGain.has_value(), std::memory_order_relaxed);
    if (volumeInit.linearGain) {
        m_cachedGain.store(*volumeInit.linearGain, std::memory_order_relaxed);
    }
    m_cachedMuted.store(volumeInit.muted, std::memory_order_relaxed);

    auto *state = m_sink.m_pw.get();
    PwPropertiesPtr props(pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr));

    const QString appName = QGuiApplication::applicationDisplayName();
    const QByteArray appNameUtf8 = appName.toUtf8();
    const char *nodeName = appNameUtf8.isEmpty() ? "DragonMediaBackend" : appNameUtf8.constData();

    pw_properties_set(props.get(), PW_KEY_APP_NAME, nodeName);
    pw_properties_set(props.get(), PW_KEY_NODE_NAME, nodeName);
    pw_properties_set(props.get(), PW_KEY_NODE_DESCRIPTION, nodeName);

    const QString mediaName = m_sink.resolvedStreamName();
    const QByteArray mediaNameUtf8 = mediaName.toUtf8();
    pw_properties_set(props.get(), PW_KEY_MEDIA_NAME, mediaNameUtf8.constData());

    pw_properties_set(props.get(), PW_KEY_NODE_ALWAYS_PROCESS, "true");
    if (m_sampleRate > 0) {
        pw_properties_setf(props.get(), PW_KEY_NODE_RATE, "1/%d", m_sampleRate);
    }

    const QString appIconName = AudioSink::applicationIconName();
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

    {
        PwThreadLoopLock lock(state->loop.get());
        m_stream->handle.reset(pw_stream_new(state->core.get(), nodeName, props.release()));
        if (!m_stream->handle) {
            qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to create stream";
            return false;
        }
        pw_stream_add_listener(m_stream->handle.get(), &m_streamListener, &s_streamEvents, this);
    }

    uint8_t podBuffer[PW_POD_BUFFER_LENGTH];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

    spa_audio_info_raw audioInfo = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32,
                                                           .flags = 0,
                                                           .rate = static_cast<uint32_t>(m_sampleRate),
                                                           .channels = static_cast<uint32_t>(m_channels),
                                                           .position = {});

    // Explicit channel positions for mono and stereo.
    // For >2 channels, PipeWire infers positions from the channel count.
    if (m_channels == 1) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (m_channels == 2) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
        audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    const spa_pod *params = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audioInfo);
    if (!params) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to build audio format pod";
        return false;
    }

    constexpr auto streamFlags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

    // Mark open before pw_stream_connect() / initial control setup so early
    // callbacks observe an open stream cleanly; reset on failure below.
    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_seq_cst);

    {
        // pw_stream_connect() requires the thread-loop lock: calling it from
        // an unlocked context corrupts the client-server protocol.
        PwThreadLoopLock lock(state->loop.get());
        int res = pw_stream_connect(m_stream->handle.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, &params, 1);
        if (res != 0) {
            m_open.store(false, std::memory_order_seq_cst);
            qCCritical(dragonMediaBackendAudio) << "PipeWire: failed to connect stream:" << res;
            return false;
        }

        if (volumeInit.linearGain) {
            setChannelVolumes(*volumeInit.linearGain);
        }
        if (volumeInit.muted) {
            float muteVal = 1.0f;
            pw_stream_set_control(m_stream->handle.get(), SPA_PROP_mute, 1, &muteVal, 0);
        }
    }

    return true;
}

bool PipeWireAudioSinkStream::closeStream()
{
    m_open.store(false, std::memory_order_seq_cst);
    m_paused.store(false, std::memory_order_release);

    if (!m_sink.m_pw->loop || !m_stream->handle) {
        return true;
    }

    m_quiesce.beginClose();
    const bool quiesced = m_quiesce.waitForIdle(CallbackQuiesce::kDefaultCloseTimeout);
    if (!quiesced) {
        qCCritical(dragonMediaBackendAudio) << "PipeWire stream close(): callbacks did not quiesce within" << CallbackQuiesce::kDefaultCloseTimeout.count()
                                            << "ms; leaking the pw stream (zombie state) to avoid a use-after-free";
        m_sink.m_poisoned = true;
        // Deliberately leak: release the stream without destroying it, so a
        // still-running onProcess can keep dereferencing a valid object.
        m_sink.unregisterStream(this);
        std::ignore = m_stream.release();
        return false;
    }

    {
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        pw_stream_set_active(m_stream->handle.get(), false);
        spa_hook_remove(&m_streamListener);
        pw_stream_disconnect(m_stream->handle.get());
        m_stream->handle.reset();
    }
    m_sink.unregisterStream(this);
    return true;
}

void PipeWireAudioSinkStream::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_sink.m_pw->loop && m_stream->handle) {
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        pw_stream_set_active(m_stream->handle.get(), false);
    }
}

void PipeWireAudioSinkStream::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_sink.m_pw->loop && m_stream->handle) {
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        pw_stream_set_active(m_stream->handle.get(), true);
    }
}

bool PipeWireAudioSinkStream::isPaused() const
{
    return m_paused.load(std::memory_order_acquire);
}

void PipeWireAudioSinkStream::setGain(float linearGain)
{
    m_gainSetByPipeline.store(true, std::memory_order_relaxed);
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    if (m_sink.m_pw->loop && m_stream->handle && m_open.load(std::memory_order_acquire)) {
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        setChannelVolumes(linearGain);
    }
}

void PipeWireAudioSinkStream::setMuted(bool muted)
{
    m_cachedMuted.store(muted, std::memory_order_relaxed);

    if (m_sink.m_pw->loop && m_stream->handle && m_open.load(std::memory_order_acquire)) {
        float muteVal = muted ? 1.0f : 0.0f;
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        pw_stream_set_control(m_stream->handle.get(), SPA_PROP_mute, 1, &muteVal, 0);
    }
}

void PipeWireAudioSinkStream::clearStream()
{
    if (m_sink.m_pw->loop && m_stream->handle) {
        PwThreadLoopLock lock(m_sink.m_pw->loop.get());
        pw_stream_flush(m_stream->handle.get(), false);
    }
}

void PipeWireAudioSinkStream::setChannelVolumes(float linearGain)
{
    const auto ch = static_cast<uint32_t>(m_channels);
    m_volumesScratch.assign(ch, linearGain);
    pw_stream_set_control(m_stream->handle.get(), SPA_PROP_channelVolumes, ch, m_volumesScratch.data(), 0);
}

qint64 PipeWireAudioSinkStream::deviceQueuedSamples() const
{
    if (!m_sink.m_pw->loop || !m_stream->handle) {
        return 0;
    }
    PwThreadLoopLock lock(m_sink.m_pw->loop.get());
    auto *stream = m_stream->handle.get();
    if (!stream) {
        return 0;
    }

    pw_time time{};
    if (pw_stream_get_time_n(stream, &time, sizeof(time)) != 0) {
        return 0;
    }

    return static_cast<qint64>(time.queued) / sizeof(float);
}

int PipeWireAudioSinkStream::audioBufferFrames() const
{
    if (!m_sink.m_pw->loop || !m_stream->handle) {
        return -1;
    }
    PwThreadLoopLock lock(m_sink.m_pw->loop.get());
    auto *stream = m_stream->handle.get();
    if (!stream) {
        return -1;
    }

    pw_time time{};
    if (pw_stream_get_time_n(stream, &time, sizeof(time)) != 0) {
        return -1;
    }

    if (m_channels <= 0) {
        return -1;
    }
    return static_cast<int>(time.queued / (m_channels * sizeof(float)));
}

std::optional<std::chrono::microseconds> PipeWireAudioSinkStream::audioBufferDuration() const
{
    const int frames = audioBufferFrames();
    if (frames < 0 || m_sampleRate <= 0) {
        return std::nullopt;
    }
    return std::chrono::microseconds{(static_cast<qint64>(frames) * 1000000) / m_sampleRate};
}

void PipeWireAudioSinkStream::onProcess(void *userdata)
{
    auto *self = static_cast<PipeWireAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }

    // Quiesce handshake with closeStream(): onProcess runs on the pw data
    // thread outside the loop lock, so it must bail out before touching the
    // stream when close is in progress. The guard's destructor is a
    // fetch_sub plus an atomic notify, never a mutex, so it is safe on this
    // RT path.
    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing()) {
        return;
    }

    if (!self->m_open.load(std::memory_order_seq_cst)) {
        return;
    }

    thread_local bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        ThreadName::set("dragon-pw-cb");
        audioThreadNamed = true;
    }

    auto *stream = self->m_stream->handle.get();
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

    const int channels = self->m_channels;
    if (channels <= 0) {
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    const uint32_t maxBytes = spaBuf->datas[0].maxsize;
    const uint32_t maxFramesFromSize = maxBytes / static_cast<uint32_t>(channels * sizeof(float));

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

    const size_t maxSamples = requestedFrames * static_cast<size_t>(channels);

    pw_time pwt{};
    qint64 latencyUs = 0;
    if (pw_stream_get_time_n(stream, &pwt, sizeof(pwt)) == 0 && pwt.rate.denom > 0) {
        latencyUs = (pwt.delay * 1'000'000LL * pwt.rate.num) / pwt.rate.denom;
    }

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(latencyUs);

    auto pcm = self->renderer().render(maxSamples, pts);

    auto *dst = static_cast<float *>(spaBuf->datas[0].data);

    if (pcm.empty() && self->renderer().tryClaimDrained()) {
        spaBuf->datas[0].chunk->size = 0;
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
        pwBuf->size = 0;
        pw_stream_queue_buffer(stream, pwBuf);
        pw_stream_flush(stream, true);
        return;
    }

    if (pcm.empty() && self->renderer().endOfStreamArmed()) {
        // Post-EOS (drain armed or already claimed): queue an empty buffer
        // instead of a full quantum of silence. This keeps the stream
        // consuming (and eventually idle) without perpetually rendering
        // zeros, which is what made PW_KEY_NODE_ALWAYS_PROCESS burn CPU
        // after a drain. The underrun path below keeps queueing silence so
        // ordinary mid-stream starvation does not kill the stream.
        spaBuf->datas[0].chunk->size = 0;
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
        pwBuf->size = 0;
        pw_stream_queue_buffer(stream, pwBuf);
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

void PipeWireAudioSinkStream::handleSinkInputVolumes(float avgGain)
{
    const bool isFirstReport = !m_firstVolumeReportSeen.exchange(true, std::memory_order_relaxed);
    const bool neverSetGain = !m_gainSetByPipeline.load(std::memory_order_relaxed);

    float cached = m_cachedGain.load(std::memory_order_relaxed);
    if (qAbs(avgGain - cached) < 0.001f) {
        return;
    }
    m_cachedGain.store(avgGain, std::memory_order_relaxed);

    if (isFirstReport && neverSetGain) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire stream-restore volume via control_info, avgGain:" << avgGain;
        QMetaObject::invokeMethod(
            this,
            [this, avgGain]() {
                Q_EMIT externalVolumeRestored(avgGain);
            },
            Qt::QueuedConnection);
    } else {
        qCDebug(dragonMediaBackendAudio) << "PipeWire external volume change via control_info, avgGain:" << avgGain;
        QMetaObject::invokeMethod(
            this,
            [this, avgGain]() {
                Q_EMIT externalVolumeChanged(avgGain);
            },
            Qt::QueuedConnection);
    }
}

void PipeWireAudioSinkStream::handleSinkInputMuted(bool muted)
{
    if (muted == m_cachedMuted.load(std::memory_order_relaxed)) {
        return;
    }
    m_cachedMuted.store(muted, std::memory_order_relaxed);

    qCDebug(dragonMediaBackendAudio) << "PipeWire external mute change, muted:" << muted;
    QMetaObject::invokeMethod(
        this,
        [this, muted]() {
            Q_EMIT externalMutedChanged(muted);
        },
        Qt::QueuedConnection);
}

void PipeWireAudioSinkStream::onControlInfo(void *userdata, uint32_t id, const pw_stream_control *control)
{
    auto *self = static_cast<PipeWireAudioSinkStream *>(userdata);
    if (!self || !control || control->n_values == 0) {
        return;
    }

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing() || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PipeWire control_info id:" << id;

    if (id == SPA_PROP_channelVolumes) {
        float sum = 0.0f;
        for (uint32_t i = 0; i < control->n_values; ++i) {
            sum += control->values[i];
        }
        self->handleSinkInputVolumes(sum / static_cast<float>(control->n_values));
    } else if (id == SPA_PROP_mute) {
        self->handleSinkInputMuted(control->values[0] >= 0.5f);
    }
}

void PipeWireAudioSinkStream::onParamChanged(void *userdata, uint32_t id, const spa_pod *param)
{
    auto *self = static_cast<PipeWireAudioSinkStream *>(userdata);
    if (!self || !param) {
        return;
    }

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing() || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "PipeWire param_changed id:" << id;

    if (id != SPA_PARAM_Props) {
        return;
    }

    // SPA_PROP_volume is deliberately not handled here: stream gain is
    // owned through channelVolumes (set at creation and observed in
    // onControlInfo). Handling volume here as well made the same external
    // change emit externalVolumeChanged twice, potentially with differing
    // values (mono volume vs averaged channel volumes).

    bool muted = false;
    if (const spa_pod_prop *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute); prop != nullptr && spa_pod_get_bool(&prop->value, &muted) == 0) {
        qCDebug(dragonMediaBackendAudio) << "PipeWire param_changed SPA_PROP_mute:" << muted;
        self->handleSinkInputMuted(muted);
    }
}

void PipeWireAudioSinkStream::onStateChanged(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    auto *self = static_cast<PipeWireAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }
    qCDebug(dragonMediaBackendAudio) << "PipeWire stream state changed from" << pw_stream_state_as_string(old) << "to" << pw_stream_state_as_string(state);

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing() || !self->m_open.load(std::memory_order_acquire)) {
        return;
    }

    if (state == PW_STREAM_STATE_ERROR) {
        const QString errorMsg = error ? QString::fromUtf8(error) : QStringLiteral("PipeWire stream error");
        qCCritical(dragonMediaBackendAudio) << "PipeWire stream entered ERROR state:" << errorMsg;
        QMetaObject::invokeMethod(
            self,
            [self, errorMsg]() {
                if (self->m_open.load(std::memory_order_acquire)) {
                    Q_EMIT self->errorOccurred(errorMsg);
                }
            },
            Qt::QueuedConnection);
    }
}

void PipeWireAudioSinkStream::onDrained(void *userdata)
{
    auto *self = static_cast<PipeWireAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
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

#include "pipewireaudiosink.moc"
