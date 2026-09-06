/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonsdlaudiosink.h"

#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>

#include "dragonmediabackend_audio_logging.h"

#include <KPluginFactory>

K_PLUGIN_CLASS_WITH_JSON(DragonSdlAudioSink, "sdl_sink.json")

#include <QGuiApplication>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include "dragonthreadname.h"
#include <chrono>
#include <thread>

// specifically on Windows the resampler never goes to zero
constexpr int kMaxResamplerResidualBytes = 256;

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

DragonSdlAudioSink::DragonSdlAudioSink(QObject *parent, const QVariantList &args)
    : DragonAudioSink(parent)
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

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_Init(SDL_INIT_AUDIO) failed:" << SDL_GetError();
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
    }
}

DragonSdlAudioSink::~DragonSdlAudioSink()
{
    DragonSdlAudioSink::close();
    SDL_Quit();
}

bool DragonSdlAudioSink::probe()
{
    qCDebug(dragonMediaBackendAudio) << "SDL probe() SDL_Init already called in constructor";
    return true; // SDL_Init succeeded during construction
}

void DragonSdlAudioSink::open(int sampleRate, int channels)
{
    qCDebug(dragonMediaBackendAudio) << "open" << sampleRate << channels;

    if (m_session.load(std::memory_order_acquire) != nullptr) {
        qCDebug(dragonMediaBackendAudio) << "open() called while already open closing old session";
        close();
    }

    setFormat(sampleRate, channels);
    reset();

    auto *session = new AudioSession;

    const QString openStreamName = resolvedStreamName();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, openStreamName.toUtf8().constData());

    const SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, sampleRate};

    preAllocateCallbackBuffer(spec.freq * spec.channels);

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

    if (!SDL_SetAudioStreamGetCallback(stream, &DragonSdlAudioSink::audioStreamCallback, this)) {
        qCCritical(dragonMediaBackendAudio) << "SDL_SetAudioStreamGetCallback FAILED:" << SDL_GetError();
        SDL_CloseAudioDevice(deviceId);
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    SDL_ResumeAudioDevice(deviceId);

    m_session.store(session, std::memory_order_release);
    qCDebug(dragonMediaBackendAudio) << "SDL audio device opened";
}

void DragonSdlAudioSink::close()
{
    qCDebug(dragonMediaBackendAudio) << "close()";

    if (m_drainTimer) {
        m_drainTimer->stop();
    }

    auto *oldSession = m_session.exchange(nullptr, std::memory_order_acq_rel);

    if (oldSession) {
        if (oldSession->deviceId != 0) {
            SDL_PauseAudioDevice(oldSession->deviceId);
            SDL_CloseAudioDevice(oldSession->deviceId);
        }
        if (oldSession->stream) {
            SDL_DestroyAudioStream(oldSession->stream);
        }
    }

    {
        std::unique_lock lock(m_callbackDoneMutex);
        m_callbackDoneCv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
            return m_activeCallbacks.load(std::memory_order_acquire) == 0;
        });
    }

    delete oldSession;
    qCDebug(dragonMediaBackendAudio) << "close() complete";
}

void DragonSdlAudioSink::pause()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->deviceId != 0) {
            SDL_PauseAudioDevice(session->deviceId);
        }
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, 0.0f);
        }
    }
}

void DragonSdlAudioSink::resume()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, m_cachedGain.load(std::memory_order_relaxed));
        }
        if (session->deviceId != 0) {
            SDL_ResumeAudioDevice(session->deviceId);
        }
    }
}

void DragonSdlAudioSink::setGain(float linearGain)
{
    m_cachedGain.store(linearGain, std::memory_order_relaxed);
    if (auto *session = m_session.load(std::memory_order_acquire); session && session->stream) {
        SDL_SetAudioStreamGain(session->stream, linearGain);
    }
}

qint64 DragonSdlAudioSink::deviceQueuedSamples() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || !session->stream) {
        return 0;
    }
    return SDL_GetAudioStreamQueued(session->stream) / static_cast<int>(sizeof(float));
}

void DragonSdlAudioSink::setStreamName(const QString &name)
{
    DragonAudioSink::setStreamName(name);

    if (name.isEmpty()) {
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME);
    } else {
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, name.toUtf8().constData());
    }
}

void DragonSdlAudioSink::notifyDecodeFinished()
{
    DragonAudioSink::notifyDecodeFinished();

    if (!m_drainTimer) {
        m_drainTimer = new QTimer(this);
        connect(m_drainTimer, &QTimer::timeout, this, [this]() {
            auto *session = m_session.load(std::memory_order_acquire);
            if (session && session->stream) {
                const int queuedBytes = SDL_GetAudioStreamQueued(session->stream);
                const int availableBytes = SDL_GetAudioStreamAvailable(session->stream);
                if (availableBytes == 0 && queuedBytes <= kMaxResamplerResidualBytes) {
                    if (m_drain.tryClaimDrain()) {
                        m_drain.consumeEmission();
                        m_drainTimer->stop();
                        qCDebug(dragonMediaBackendAudio) << "drain complete queuedBytes=" << queuedBytes;
                        Q_EMIT drained();
                    }
                }
            }
        });
    }
    m_drainTimer->start(kDrainPollMs);
}

void DragonSdlAudioSink::resetDrainState()
{
    if (m_drainTimer) {
        m_drainTimer->stop();
    }
    DragonAudioSink::resetDrainState();
}

bool DragonSdlAudioSink::isDeviceOpen() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    return session != nullptr && session->deviceId != 0 && session->stream != nullptr;
}

void DragonSdlAudioSink::clearStream()
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (session && session->stream) {
        SDL_ClearAudioStream(session->stream);
    }
}

bool DragonSdlAudioSink::isPaused() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || session->deviceId == 0) {
        return false;
    }
    return SDL_AudioDevicePaused(session->deviceId);
}

int DragonSdlAudioSink::audioBufferFrames() const
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

int DragonSdlAudioSink::audioBufferUs() const
{
    const int frames = audioBufferFrames();
    if (frames <= 0) {
        return frames;
    }

    if (currentSampleRate() <= 0) {
        return -1;
    }

    return static_cast<int>((static_cast<qint64>(frames) * 1000000) / currentSampleRate());
}

void SDLCALL DragonSdlAudioSink::audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int)
{
    auto *self = static_cast<DragonSdlAudioSink *>(userdata);
    if (!self)
        return;

    thread_local bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        DragonThreadName::set("dragon-audio");
        audioThreadNamed = true;
    }

    self->m_activeCallbacks.fetch_add(1, std::memory_order_relaxed);
    struct Guard {
        DragonSdlAudioSink *self;
        ~Guard()
        {
            self->m_activeCallbacks.fetch_sub(1, std::memory_order_relaxed);
            self->m_callbackDoneCv.notify_one();
        }
    };
    Guard guard{self};

    const auto *session = self->m_session.load(std::memory_order_acquire);
    if (!session)
        return;

    if (additional_amount <= 0)
        return;

    const int channels = self->currentChannels();
    const int sampleRate = self->currentSampleRate();
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

    auto pcm = self->processAudioCallback(floatsNeeded, pts);

    if (!pcm.empty()) {
        SDL_PutAudioStreamData(stream, pcm.data(), static_cast<int>(pcm.size() * sizeof(float)));
    } else {
        qCDebug(dragonMediaBackendAudio) << "STARVATION additional=" << additional_amount << "floatsNeeded=" << floatsNeeded;
    }
}

#include "dragonsdlaudiosink.moc"
