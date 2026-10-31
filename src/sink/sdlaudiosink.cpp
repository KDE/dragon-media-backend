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
#include "mixer/mixer.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(SdlAudioSink, "sdl_sink.json")

#include <QGuiApplication>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include "threadname.h"
#include <chrono>
#include <thread>

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
    SdlAudioSink::close();
    // A poisoned close deliberately leaks the native session (and the device
    // behind it): tearing the whole SDL subsystem down underneath a
    // still-running callback would only trade one use-after-free for another.
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

void SdlAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendAudio) << "open" << sampleRate << channels;

    if (m_session.load(std::memory_order_acquire) != nullptr) {
        qCDebug(dragonMediaBackendAudio) << "open() called while already open closing old session";
        close();
    }

    auto *session = new AudioSession;

    const QString openStreamName = resolvedStreamName();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, openStreamName.toUtf8().constData());

    const SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, sampleRate};

    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!stream) {
        qCCritical(dragonMediaBackendAudio) << "SDL_CreateAudioStream failed:" << SDL_GetError();
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }
    session->stream = stream;

    SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (deviceId == 0) {
        qCCritical(dragonMediaBackendAudio) << "SDL_OpenAudioDevice failed:" << SDL_GetError();
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }
    session->deviceId = deviceId;

    if (!SDL_BindAudioStreams(deviceId, &stream, 1)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_BindAudioStreams failed:" << SDL_GetError();
        SDL_CloseAudioDevice(deviceId);
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    SDL_SetAudioStreamGain(stream, m_cachedGain.load(std::memory_order_relaxed));

    // Publish the session BEFORE registering the callback and resuming the
    // device: once the device is running, the RT callback can fire at any
    // moment and must never observe a null (or stale) session.
    m_session.store(session, std::memory_order_seq_cst);

    if (!SDL_SetAudioStreamGetCallback(stream, &SdlAudioSink::audioStreamCallback, this)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_SetAudioStreamGetCallback FAILED:" << SDL_GetError();
        m_session.store(nullptr, std::memory_order_seq_cst);
        SDL_CloseAudioDevice(deviceId);
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    SDL_ResumeAudioDevice(deviceId);

    qCDebug(dragonMediaBackendAudio) << "SDL audio device opened";
}

void SdlAudioSink::close()
{
    qCDebug(dragonMediaBackendAudio) << "close()";

    // Serialize against the other main-thread control methods.
    std::lock_guard lock(m_mainMutex);

    auto *oldSession = m_session.exchange(nullptr, std::memory_order_seq_cst);

    if (oldSession && oldSession->deviceId != 0) {
        SDL_PauseAudioDevice(oldSession->deviceId);
    }

    // Wait for active callbacks to finish BEFORE tearing down the session.
    // New callbacks bail out at their entry while close() is in progress, so
    // this is bounded.
    m_quiesce.beginClose();
    const bool quiesced = m_quiesce.waitForIdle(std::chrono::milliseconds(500));
    if (!quiesced) {
        m_poisoned = true;
        qCCritical(dragonMediaBackendAudio)
            << "SDL close(): callback did not quiesce within 500ms; leaking the native audio session (zombie state) to avoid a use-after-free";
        // Deliberately leak the session and everything it owns: the device and
        // the stream keep living with the (stuck) callback, which may still
        // dereference them. close() stays marked as in progress so any future
        // callback bails out immediately.
        return;
    }
    m_quiesce.endClose();

    if (oldSession) {
        if (oldSession->deviceId != 0) {
            SDL_CloseAudioDevice(oldSession->deviceId);
        }
        if (oldSession->stream) {
            SDL_DestroyAudioStream(oldSession->stream);
        }
    }

    delete oldSession;
    qCDebug(dragonMediaBackendAudio) << "close() complete";
}

void SdlAudioSink::pause()
{
    std::lock_guard lock(m_mainMutex);

    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->deviceId != 0) {
            SDL_PauseAudioDevice(session->deviceId);
        }
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, 0.0f);
        }
    }
}

void SdlAudioSink::resume()
{
    std::lock_guard lock(m_mainMutex);

    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, m_cachedGain.load(std::memory_order_relaxed));
        }
        if (session->deviceId != 0) {
            SDL_ResumeAudioDevice(session->deviceId);
        }
    }
}

void SdlAudioSink::setGain(float linearGain)
{
    m_cachedGain.store(linearGain, std::memory_order_relaxed);

    std::lock_guard lock(m_mainMutex);

    if (auto *session = m_session.load(std::memory_order_acquire); session && session->stream) {
        SDL_SetAudioStreamGain(session->stream, linearGain);
    }
}

qint64 SdlAudioSink::deviceQueuedSamples() const
{
    std::lock_guard lock(m_mainMutex);

    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return 0;
    }
    return SDL_GetAudioStreamQueued(session->stream) / static_cast<int>(sizeof(float));
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

bool SdlAudioSink::isDeviceOpen() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    return session != nullptr && session->deviceId != 0 && session->stream != nullptr;
}

void SdlAudioSink::clearStream()
{
    std::lock_guard lock(m_mainMutex);

    auto *session = m_session.load(std::memory_order_acquire);
    if (session && session->stream) {
        SDL_ClearAudioStream(session->stream);
    }
}

bool SdlAudioSink::isPaused() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || session->deviceId == 0) {
        return false;
    }
    return SDL_AudioDevicePaused(session->deviceId);
}

int SdlAudioSink::audioBufferFrames() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || !session->stream || !session->deviceId) {
        return -1;
    }

    SDL_AudioSpec spec;
    int sampleFrames = 0;
    if (!SDL_GetAudioDeviceFormat(session->deviceId, &spec, &sampleFrames)) {
        return -1;
    }
    return sampleFrames;
}

std::optional<std::chrono::microseconds> SdlAudioSink::audioBufferDuration() const
{
    const int frames = audioBufferFrames();
    if (frames < 0 || mixer()->clock().currentSampleRate() <= 0) {
        return std::nullopt;
    }

    return std::chrono::microseconds{(static_cast<qint64>(frames) * 1000000) / mixer()->clock().currentSampleRate()};
}

void SDLCALL SdlAudioSink::audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int)
{
    auto *self = static_cast<SdlAudioSink *>(userdata);
    if (!self)
        return;

    thread_local bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        ThreadName::set("dragon-audio");
        audioThreadNamed = true;
    }

    // Quiesce handshake with close(): the destructor is a fetch_sub plus an
    // atomic notify, never a mutex, so it is safe on this RT path. While
    // close() is in progress, bail out before touching the session.
    CallbackQuiesce::ScopedCallback guard(self->m_quiesce);
    if (guard.closing()) {
        return;
    }

    const auto *session = self->m_session.load(std::memory_order_seq_cst);
    if (!session)
        return;

    if (additional_amount <= 0)
        return;

    if (!self->mixer())
        return;

    const int channels = self->mixer()->clock().currentChannels();
    const int sampleRate = self->mixer()->clock().currentSampleRate();
    if (channels <= 0 || sampleRate <= 0)
        return;

    const size_t floatsNeeded = (static_cast<size_t>(additional_amount) / sizeof(float)) / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    if (floatsNeeded == 0)
        return;

    int queuedBytes = SDL_GetAudioStreamQueued(stream);
    int softwareFrames = queuedBytes / (channels * static_cast<int>(sizeof(float)));
    SDL_AudioSpec spec;
    int hardwareFrames = 0;
    SDL_GetAudioDeviceFormat(session->deviceId, &spec, &hardwareFrames);
    int totalLatencyFrames = softwareFrames + hardwareFrames;
    float latencySeconds = static_cast<float>(totalLatencyFrames) / static_cast<float>(sampleRate);

    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto pts = std::chrono::duration_cast<std::chrono::microseconds>(now) + std::chrono::microseconds(static_cast<qint64>(latencySeconds * 1000000.0f));

    auto pcm = self->mixer()->render(floatsNeeded, pts);

    if (!pcm.empty()) {
        SDL_PutAudioStreamData(stream, pcm.data(), static_cast<int>(pcm.size() * sizeof(float)));
        return;
    }

    if (self->mixer()->clock().endOfStreamPending()) {
        SDL_FlushAudioStream(stream);
        if (SDL_GetAudioStreamQueued(stream) > 0 || SDL_GetAudioStreamAvailable(stream) > 0) {
            return;
        }
    }

    if (self->mixer()->clock().tryClaimDrained()) {
        QMetaObject::invokeMethod(
            self,
            [self]() {
                if (!self->mixer()->clock().takeDrainedEmission()) {
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
