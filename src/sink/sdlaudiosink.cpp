/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "sdlaudiosink.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>

#include "dragonmediabackend_audio_logging.h"

#include <KLocalizedString>
#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(SdlAudioSink, "sdl_sink.json")

#include <QGuiApplication>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#include "threadname.h"
#include <chrono>
#include <utility>

using namespace std::chrono_literals;

static void SDLLogOutput(void *userdata, int category, SDL_LogPriority priority, const char *message)
{
    Q_UNUSED(userdata);
    Q_UNUSED(category);

    switch (priority) {
    case SDL_LOG_PRIORITY_VERBOSE:
    case SDL_LOG_PRIORITY_DEBUG:
    case SDL_LOG_PRIORITY_TRACE:
        qCDebug(dragonMediaBackendAudio) << "SDL:" << message;
        break;
    case SDL_LOG_PRIORITY_INFO:
        qCInfo(dragonMediaBackendAudio) << "SDL:" << message;
        break;
    case SDL_LOG_PRIORITY_WARN:
        qCWarning(dragonMediaBackendAudio) << "SDL:" << message;
        break;
    case SDL_LOG_PRIORITY_ERROR:
    case SDL_LOG_PRIORITY_CRITICAL:
        qCCritical(dragonMediaBackendAudio) << "SDL:" << message;
        break;
    default:
        qCDebug(dragonMediaBackendAudio) << "SDL:" << message;
        break;
    }
}

SdlAudioSink::SdlAudioSink(QObject *parent, const QVariantList &args)
    : AudioSink(parent)
{
    Q_UNUSED(args);
    SDL_SetLogOutputFunction(SDLLogOutput, nullptr);

    SDL_SetHint(SDL_HINT_APP_NAME, QGuiApplication::applicationDisplayName().toUtf8().constData());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");

    SDL_SetMainReady();

    const QString appId = applicationIconName();
    if (!appId.isEmpty()) {
        const QByteArray appIdUtf8 = appId.toUtf8();
        SDL_SetHint(SDL_HINT_APP_ID, appIdUtf8.constData());
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_ICON_NAME, appIdUtf8.constData());
    }

#ifdef Q_OS_ANDROID
    // don't stop playing when android app isn't on screen
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
#endif

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_Init(SDL_INIT_AUDIO) failed:" << SDL_GetError();
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
    }
}

SdlAudioSink::~SdlAudioSink()
{
    {
        std::lock_guard lock(m_mainMutex);
        if (m_deviceId != 0 && !m_poisoned) {
            closeDeviceLocked();
        }
    }
    // A poisoned sink deliberately leaks the native device: tearing the
    // whole SDL subsystem down underneath a still-running callback would
    // only trade one use-after-free for another.
    if (!m_poisoned) {
        SDL_Quit();
    }
}

bool SdlAudioSink::probe()
{
    qCDebug(dragonMediaBackendAudio) << "SDL probe() checking SDL_WasInit";
    return SDL_WasInit(SDL_INIT_AUDIO) != 0;
}

std::size_t SdlAudioSink::preferredCallbackSamples(int sampleRate, int channels) const
{
    return static_cast<std::size_t>(sampleRate) * static_cast<std::size_t>(channels);
}

bool SdlAudioSink::openDeviceLocked()
{
    if (m_deviceId != 0) {
        return true;
    }

    // Request F32 (the backend's standard PCM format) while preserving the
    // device's probed default sample rate and channel count, since SDL mixes
    // all streams on this shared device.
    SDL_AudioSpec spec{};
    const bool haveDefaultSpec = SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr);
    if (haveDefaultSpec) {
        spec.format = SDL_AUDIO_F32;
    }

    SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, haveDefaultSpec ? &spec : nullptr);
    if (deviceId == 0) {
        qCCritical(dragonMediaBackendAudio) << "SDL_OpenAudioDevice failed:" << SDL_GetError();
        return false;
    }
    m_deviceId = deviceId;
    qCDebug(dragonMediaBackendAudio) << "SDL audio device opened";
    return true;
}

void SdlAudioSink::closeDeviceLocked()
{
    if (m_deviceId != 0) {
        SDL_PauseAudioDevice(m_deviceId);
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
        qCDebug(dragonMediaBackendAudio) << "SDL audio device closed (last stream)";
    }
}

std::expected<std::unique_ptr<AudioSinkStream>, QString>
SdlAudioSink::createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit)
{
    qCDebug(dragonMediaBackendAudio) << "SDL createStream" << sampleRate << channels;

    std::lock_guard lock(m_mainMutex);

    if (m_poisoned) {
        return std::unexpected(i18n("SDL: audio output is in a zombie state after a failed close"));
    }

    if (!openDeviceLocked()) {
        return std::unexpected(QString::fromUtf8(SDL_GetError()));
    }

    // SDL reads the stream-name hint when the stream is created.
    const QString openStreamName = resolvedStreamName();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, openStreamName.toUtf8().constData());

    const SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, sampleRate};
    SDL_AudioStream *rawStream = SDL_CreateAudioStream(&spec, nullptr);
    if (!rawStream) {
        qCCritical(dragonMediaBackendAudio) << "SDL_CreateAudioStream failed:" << SDL_GetError();
        if (m_liveStreams == 0) {
            closeDeviceLocked();
        }
        return std::unexpected(QString::fromUtf8(SDL_GetError()));
    }

    auto stream = std::make_unique<SdlAudioSinkStream>(*this, rawStream, m_deviceId, sampleRate, channels, renderer);
    stream->m_gain = volumeInit.linearGain.value_or(1.0f);
    stream->m_muted = volumeInit.muted;
    SDL_SetAudioStreamGain(rawStream, volumeInit.muted ? 0.0f : volumeInit.linearGain.value_or(1.0f));

    if (!SDL_BindAudioStreams(m_deviceId, &rawStream, 1)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_BindAudioStreams failed:" << SDL_GetError();
        SDL_DestroyAudioStream(rawStream);
        if (m_liveStreams == 0) {
            closeDeviceLocked();
        }
        return std::unexpected(QString::fromUtf8(SDL_GetError()));
    }
    ++m_liveStreams;

    // Registering the callback last: once it is set, the device thread may
    // call into the stream object at any moment.
    if (!SDL_SetAudioStreamGetCallback(rawStream, &SdlAudioSinkStream::getCallback, stream.get())) {
        qCCritical(dragonMediaBackendAudio) << "SDL_SetAudioStreamGetCallback failed:" << SDL_GetError();
        SDL_UnbindAudioStreams(&rawStream, 1);
        --m_liveStreams;
        SDL_DestroyAudioStream(rawStream);
        if (m_liveStreams == 0) {
            closeDeviceLocked();
        }
        return std::unexpected(QString::fromUtf8(SDL_GetError()));
    }

    SDL_ResumeAudioDevice(m_deviceId);
    return stream;
}

void SdlAudioSink::streamClosed(SdlAudioSinkStream *stream, bool quiesced, bool wasPaused)
{
    std::lock_guard lock(m_mainMutex);
    Q_UNUSED(stream);
    if (m_liveStreams > 0) {
        --m_liveStreams;
    }
    if (wasPaused && m_pausedStreams > 0) {
        --m_pausedStreams;
    }
    if (m_liveStreams == 0) {
        m_pausedStreams = 0;
    }
    if (!quiesced) {
        // The stuck callback may still dereference the native stream (and
        // the whole voice behind it): keep the device alive too.
        m_poisoned = true;
        return;
    }
    if (m_liveStreams == 0 && m_deviceId != 0) {
        closeDeviceLocked();
    } else if (m_deviceId != 0 && m_pausedStreams >= m_liveStreams) {
        SDL_PauseAudioDevice(m_deviceId);
    }
}

void SdlAudioSink::streamPauseChanged(bool paused)
{
    std::lock_guard lock(m_mainMutex);
    if (m_deviceId == 0) {
        return;
    }
    if (paused) {
        if (++m_pausedStreams >= m_liveStreams && m_liveStreams > 0) {
            SDL_PauseAudioDevice(m_deviceId);
        }
    } else {
        m_pausedStreams = std::max(0, m_pausedStreams - 1);
        if (m_pausedStreams < m_liveStreams) {
            SDL_ResumeAudioDevice(m_deviceId);
        }
    }
}

void SdlAudioSink::setStreamName(const QString &name)
{
    AudioSink::setStreamName(name);

    if (name.isEmpty()) {
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME);
    } else {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, name.toUtf8().constData());
    }
}

SdlAudioSinkStream::SdlAudioSinkStream(SdlAudioSink &sink,
                                       SDL_AudioStream *stream,
                                       SDL_AudioDeviceID deviceId,
                                       int sampleRate,
                                       int channels,
                                       AudioSinkStreamRenderer &renderer)
    : AudioSinkStream(renderer)
    , m_sink(sink)
    , m_stream(stream)
    , m_deviceId(deviceId)
    , m_sampleRate(sampleRate)
    , m_channels(channels)
{
}

void SdlAudioSinkStream::pause()
{
    if (!m_paused.exchange(true, std::memory_order_acq_rel)) {
        m_sink.streamPauseChanged(true);
    }
}

void SdlAudioSinkStream::resume()
{
    if (m_paused.exchange(false, std::memory_order_acq_rel)) {
        m_sink.streamPauseChanged(false);
    }
}

bool SdlAudioSinkStream::isPaused() const
{
    return m_paused.load(std::memory_order_acquire);
}

void SdlAudioSinkStream::setGain(float linearGain)
{
    std::lock_guard lock(m_sink.m_mainMutex);
    m_gain = linearGain;
    applyStreamGainLocked();
}

void SdlAudioSinkStream::setMuted(bool muted)
{
    std::lock_guard lock(m_sink.m_mainMutex);
    m_muted = muted;
    applyStreamGainLocked();
}

void SdlAudioSinkStream::applyStreamGainLocked()
{
    if (m_stream) {
        SDL_SetAudioStreamGain(m_stream, m_muted ? 0.0f : m_gain);
    }
}

qint64 SdlAudioSinkStream::deviceQueuedSamples() const
{
    std::lock_guard lock(m_sink.m_mainMutex);
    if (!m_stream) {
        return 0;
    }
    return SDL_GetAudioStreamQueued(m_stream) / static_cast<int>(sizeof(float));
}

void SdlAudioSinkStream::clearStream()
{
    std::lock_guard lock(m_sink.m_mainMutex);
    if (m_stream) {
        SDL_ClearAudioStream(m_stream);
    }
}

int SdlAudioSinkStream::audioBufferFrames() const
{
    std::lock_guard lock(m_sink.m_mainMutex);
    if (!m_stream || m_deviceId == 0) {
        return -1;
    }

    SDL_AudioSpec spec;
    int sampleFrames = 0;
    if (!SDL_GetAudioDeviceFormat(m_deviceId, &spec, &sampleFrames)) {
        return -1;
    }
    return sampleFrames;
}

std::optional<std::chrono::microseconds> SdlAudioSinkStream::audioBufferDuration() const
{
    const int frames = audioBufferFrames();
    if (frames < 0 || m_sampleRate <= 0) {
        return std::nullopt;
    }
    return std::chrono::microseconds{(static_cast<qint64>(frames) * 1000000) / m_sampleRate};
}

bool SdlAudioSinkStream::closeStream()
{
    const bool wasPaused = m_paused.exchange(false, std::memory_order_acq_rel);

    // Quiesce handshake with the get callback: the guard's destructor is a
    // fetch_sub plus an atomic notify, never a mutex, so it is safe on the
    // RT path. While closing, callbacks bail out before touching anything,
    // so this is bounded.
    m_quiesce.beginClose();
    const bool quiesced = m_quiesce.waitForIdle(500ms);
    if (!quiesced) {
        qCCritical(dragonMediaBackendAudio)
            << "SDL stream close(): callback did not quiesce within 500ms; leaking the native stream (zombie state) to avoid a use-after-free";
        // Deliberately leak the stream: it stays bound to the device, which
        // the sink also keeps alive, so a still-running callback can keep
        // dereferencing valid objects.
        m_sink.streamClosed(this, false, wasPaused);
        return false;
    }

    {
        std::lock_guard lock(m_sink.m_mainMutex);
        if (m_stream) {
            SDL_UnbindAudioStreams(&m_stream, 1);
            SDL_DestroyAudioStream(m_stream);
            m_stream = nullptr;
        }
    }

    // Closing the last stream closes the device, which flushes the disk
    // driver's capture file used by the end-to-end tests.
    m_sink.streamClosed(this, true, wasPaused);
    return true;
}

void SDLCALL SdlAudioSinkStream::getCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int)
{
    auto *self = static_cast<SdlAudioSinkStream *>(userdata);
    if (!self) {
        return;
    }

    thread_local bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        ThreadName::set("dragon-audio");
        audioThreadNamed = true;
    }

    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing()) {
        return;
    }

    // Pausing per stream outputs silence without rendering: the voice's
    // pipe, clock and envelopes freeze while the shared device keeps pulling
    // from the other (crossfading) stream.
    if (self->m_paused.load(std::memory_order_acquire)) {
        return;
    }

    if (additional_amount <= 0) {
        return;
    }

    const int channels = self->m_channels;
    const int sampleRate = self->m_sampleRate;
    if (channels <= 0 || sampleRate <= 0) {
        return;
    }

    const size_t floatsNeeded = (static_cast<size_t>(additional_amount) / sizeof(float)) / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    if (floatsNeeded == 0) {
        return;
    }

    int queuedBytes = SDL_GetAudioStreamQueued(stream);
    int softwareFrames = queuedBytes / (channels * static_cast<int>(sizeof(float)));
    SDL_AudioSpec spec;
    int hardwareFrames = 0;
    SDL_GetAudioDeviceFormat(self->m_deviceId, &spec, &hardwareFrames);
    int totalLatencyFrames = softwareFrames + hardwareFrames;
    float latencySeconds = static_cast<float>(totalLatencyFrames) / static_cast<float>(sampleRate);

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(static_cast<qint64>(latencySeconds * 1000000.0f));

    auto pcm = self->renderer().render(floatsNeeded, pts);

    if (!pcm.empty()) {
        SDL_PutAudioStreamData(stream, pcm.data(), static_cast<int>(pcm.size() * sizeof(float)));
        return;
    }

    if (self->renderer().endOfStreamPending()) {
        SDL_FlushAudioStream(stream);
        if (SDL_GetAudioStreamQueued(stream) > 0 || SDL_GetAudioStreamAvailable(stream) > 0) {
            return;
        }
    }

    if (self->renderer().tryClaimDrained()) {
        QMetaObject::invokeMethod(
            self,
            [self]() {
                if (!self->renderer().takeDrainedEmission()) {
                    return;
                }
                qCDebug(dragonMediaBackendAudio) << "drain complete, all decoded audio played out";
                Q_EMIT self->drained();
            },
            Qt::QueuedConnection);
        return;
    }

    qCDebug(dragonMediaBackendAudio) << "STARVATION additional=" << additional_amount << "floatsNeeded=" << floatsNeeded;
}

#include "sdlaudiosink.moc"
