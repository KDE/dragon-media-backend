/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonpipewireaudiosink.h"

#include "dragonsdl_audio_logging.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(DragonPipeWireAudioSink, "pipewire_sink.json")

#include <QGuiApplication>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <chrono>
#include <cstring>
#include <pthread.h>
#include <vector>

static constexpr int PW_POD_BUFFER_LENGTH = 1024;

struct DragonPipeWireAudioSink::PwState {
    pw_thread_loop *loop = nullptr;
    pw_stream *stream = nullptr;
    spa_hook streamListener{};
};

static const struct pw_stream_events s_streamEvents = [] {
    struct pw_stream_events ev{};
    ev.version = PW_VERSION_STREAM_EVENTS;
    ev.process = DragonPipeWireAudioSink::onProcess;
    return ev;
}();

DragonPipeWireAudioSink::DragonPipeWireAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
    , m_pw(std::make_unique<PwState>())
{
    Q_UNUSED(args);
    pw_init(nullptr, nullptr);
    qCDebug(dragonsdlAudio) << "PipeWire audio sink created, library version:" << pw_get_library_version();
}

DragonPipeWireAudioSink::~DragonPipeWireAudioSink()
{
    close();
    pw_deinit();
}

void DragonPipeWireAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonsdlAudio) << "PipeWire open" << sampleRate << channels;

    if (m_open.load(std::memory_order_acquire)) {
        qCDebug(dragonsdlAudio) << "open() called while already open closing old session";
        close();
    }

    setFormat(sampleRate, channels);
    reset();

    // Create thread loop
    m_pw->loop = pw_thread_loop_new("dragon-pw", nullptr);
    if (!m_pw->loop) {
        qCCritical(dragonsdlAudio) << "PipeWire: failed to create thread loop";
        Q_EMIT errorOccurred(QStringLiteral("PipeWire: failed to create thread loop"));
        return;
    }

    // Build stream properties
    auto *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music", nullptr);

    const QString appName = QGuiApplication::applicationDisplayName();
    const QByteArray appNameUtf8 = appName.toUtf8();
    const char *nodeName = appNameUtf8.isEmpty() ? "dragon-sdl" : appNameUtf8.constData();

    pw_properties_set(props, PW_KEY_APP_NAME, nodeName);
    pw_properties_set(props, PW_KEY_NODE_NAME, nodeName);
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, nodeName);
    pw_properties_set(props, PW_KEY_MEDIA_NAME, nodeName);

    // Set latency hint: 512 frames at the given sample rate
    char latencyBuf[64];
    std::snprintf(latencyBuf, sizeof(latencyBuf), "512/%d", sampleRate);
    pw_properties_set(props, PW_KEY_NODE_LATENCY, latencyBuf);

    char rateBuf[32];
    std::snprintf(rateBuf, sizeof(rateBuf), "1/%d", sampleRate);
    pw_properties_set(props, PW_KEY_NODE_RATE, rateBuf);

    pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");

    // Create the stream
    m_pw->stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_pw->loop),
                                        nodeName,
                                        props, // ownership transferred to pw_stream_new_simple
                                        &s_streamEvents,
                                        this);

    if (!m_pw->stream) {
        qCCritical(dragonsdlAudio) << "PipeWire: failed to create stream";
        pw_thread_loop_destroy(m_pw->loop);
        m_pw->loop = nullptr;
        Q_EMIT errorOccurred(QStringLiteral("PipeWire: failed to create stream"));
        return;
    }

    // Build SPA format pod: F32 interleaved
    uint8_t podBuffer[PW_POD_BUFFER_LENGTH];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

    struct spa_audio_info_raw audioInfo = {};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    audioInfo.rate = static_cast<uint32_t>(sampleRate);
    audioInfo.channels = static_cast<uint32_t>(channels);

    // Set standard channel positions
    if (channels == 1) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (channels == 2) {
        audioInfo.position[0] = SPA_AUDIO_CHANNEL_FL;
        audioInfo.position[1] = SPA_AUDIO_CHANNEL_FR;
    }

    const struct spa_pod *params = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audioInfo);
    if (!params) {
        qCCritical(dragonsdlAudio) << "PipeWire: failed to build audio format pod";
        pw_stream_destroy(m_pw->stream);
        m_pw->stream = nullptr;
        pw_thread_loop_destroy(m_pw->loop);
        m_pw->loop = nullptr;
        Q_EMIT errorOccurred(QStringLiteral("PipeWire: failed to build audio format"));
        return;
    }

    // Connect the stream
    constexpr auto streamFlags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);

    int res = pw_stream_connect(m_pw->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, streamFlags, &params, 1);

    if (res != 0) {
        qCCritical(dragonsdlAudio) << "PipeWire: failed to connect stream:" << res;
        pw_stream_destroy(m_pw->stream);
        m_pw->stream = nullptr;
        pw_thread_loop_destroy(m_pw->loop);
        m_pw->loop = nullptr;
        Q_EMIT errorOccurred(QStringLiteral("PipeWire: failed to connect stream"));
        return;
    }

    // Start the thread loop
    res = pw_thread_loop_start(m_pw->loop);
    if (res != 0) {
        qCCritical(dragonsdlAudio) << "PipeWire: failed to start thread loop:" << res;
        pw_stream_destroy(m_pw->stream);
        m_pw->stream = nullptr;
        pw_thread_loop_destroy(m_pw->loop);
        m_pw->loop = nullptr;
        Q_EMIT errorOccurred(QStringLiteral("PipeWire: failed to start thread loop"));
        return;
    }

    // Apply the current gain
    pw_thread_loop_lock(m_pw->loop);
    float vol = m_cachedGain;
    std::vector<float> vols(static_cast<size_t>(channels), vol);
    pw_stream_set_control(m_pw->stream, SPA_PROP_channelVolumes, static_cast<uint32_t>(channels), vols.data());
    pw_thread_loop_unlock(m_pw->loop);

    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    qCDebug(dragonsdlAudio) << "PipeWire audio device opened";
}

void DragonPipeWireAudioSink::close()
{
    qCDebug(dragonsdlAudio) << "PipeWire close()";

    m_open.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);

    if (m_pw->loop) {
        pw_thread_loop_stop(m_pw->loop);
    }

    if (m_pw->stream) {
        pw_stream_destroy(m_pw->stream);
        m_pw->stream = nullptr;
    }

    if (m_pw->loop) {
        pw_thread_loop_destroy(m_pw->loop);
        m_pw->loop = nullptr;
    }

    qCDebug(dragonsdlAudio) << "PipeWire close() complete";
}

void DragonPipeWireAudioSink::pause()
{
    m_paused.store(true, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        pw_stream_set_active(m_pw->stream, false);
        // Also mute gain during pause like SDL backend does
        float zero = 0.0f;
        std::vector<float> vols(static_cast<size_t>(currentChannels()), zero);
        pw_stream_set_control(m_pw->stream, SPA_PROP_channelVolumes, static_cast<uint32_t>(currentChannels()), vols.data());
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::resume()
{
    m_paused.store(false, std::memory_order_release);

    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        // Restore gain and reactivate
        float vol = m_cachedGain;
        std::vector<float> vols(static_cast<size_t>(currentChannels()), vol);
        pw_stream_set_control(m_pw->stream, SPA_PROP_channelVolumes, static_cast<uint32_t>(currentChannels()), vols.data());
        pw_stream_set_active(m_pw->stream, true);
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::setGain(float linearGain)
{
    m_cachedGain = linearGain;

    if (m_pw->loop && m_pw->stream && m_open.load(std::memory_order_acquire)) {
        pw_thread_loop_lock(m_pw->loop);
        std::vector<float> vols(static_cast<size_t>(currentChannels()), linearGain);
        pw_stream_set_control(m_pw->stream, SPA_PROP_channelVolumes, static_cast<uint32_t>(currentChannels()), vols.data());
        pw_thread_loop_unlock(m_pw->loop);
    }
}

void DragonPipeWireAudioSink::clearStream()
{
    if (m_pw->loop && m_pw->stream) {
        pw_thread_loop_lock(m_pw->loop);
        pw_stream_flush(m_pw->stream, false);
        pw_thread_loop_unlock(m_pw->loop);
    }
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

    // time.queued is the sum of pw_buffer::size fields for queued buffers.
    // In our onProcess, we set pwBuf->size to the number of frames.
    return static_cast<int64_t>(time.queued) * currentChannels();
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

    return static_cast<int>(time.queued);
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

    thread_local static bool audioThreadNamed = false;
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

    uint32_t requestedFrames = maxFramesFromSize;
    if (pwBuf->requested > 0) {
        requestedFrames = std::min(static_cast<uint32_t>(pwBuf->requested), maxFramesFromSize);
    } else {
        // If PipeWire doesn't suggest a size, clamp to a reasonable max (e.g. 1024 frames)
        // to ensure frequent callbacks and responsive position resets.
        requestedFrames = std::min(uint32_t{1024}, maxFramesFromSize);
    }

    const size_t maxSamples = requestedFrames * self->currentChannels();

    if (maxSamples == 0) {
        pw_stream_queue_buffer(stream, pwBuf);
        return;
    }

    // Compute PTS estimate for FFT sync
    const int channels = self->currentChannels();

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now);

    auto pcm = self->processAudioCallback(maxSamples, pts);

    auto *dst = static_cast<float *>(spaBuf->datas[0].data);
    if (!pcm.empty()) {
        std::memcpy(dst, pcm.data(), pcm.size() * sizeof(float));
        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->stride = static_cast<int32_t>(channels * sizeof(float));
        spaBuf->datas[0].chunk->size = static_cast<uint32_t>(pcm.size() * sizeof(float));

        // Set pw_buffer::size to number of frames for position tracking via pw_time::queued
        const auto frames = pcm.size() / static_cast<size_t>(channels);
        pwBuf->size = frames;
    } else {
        // Starvation write silence
        std::memset(dst, 0, maxSamples * sizeof(float));
        spaBuf->datas[0].chunk->offset = 0;
        spaBuf->datas[0].chunk->stride = static_cast<int32_t>(channels * sizeof(float));
        spaBuf->datas[0].chunk->size = static_cast<uint32_t>(maxSamples * sizeof(float));
        spaBuf->datas[0].chunk->flags = SPA_CHUNK_FLAG_EMPTY;
        pwBuf->size = requestedFrames;
    }

    pw_stream_queue_buffer(stream, pwBuf);
}

#include "dragonpipewireaudiosink.moc"
