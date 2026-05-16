/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiooutput.h"

#include <stdfloat>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include "dragonpipe.h"

#include "dragonsdl_audio_logging.h"
#include <QGuiApplication>
#include <QIcon>
#include <QString>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <pthread.h>
#include <ranges>
#include <thread>

using namespace Qt::StringLiterals;

DragonAudioOutput::DragonAudioOutput(QObject *parent)
    : QObject(parent)
{
    const QString iconName = QGuiApplication::windowIcon().name();
    SDL_SetHint(SDL_HINT_APP_NAME, QGuiApplication::applicationDisplayName().toUtf8().constData());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_ICON_NAME, iconName.isEmpty() ? "dragon-sdl" : iconName.toUtf8().constData());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        qCCritical(dragonsdlAudio) << "SDL_Init(SDL_INIT_AUDIO) failed:" << SDL_GetError();
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
    }
}

DragonAudioOutput::~DragonAudioOutput()
{
    stop();
    SDL_Quit();
}

void DragonAudioOutput::setAudioPipe(DragonPipe<std::float32_t> *pipe)
{
    m_audioPipe.store(pipe, std::memory_order_release);
}

void DragonAudioOutput::setFftPipe(DragonPipe<std::float32_t> *pipe)
{
    m_fftPipe.store(pipe, std::memory_order_release);
}

void DragonAudioOutput::start(int sampleRate, int channels, bool startPaused)
{
    qCDebug(dragonsdlAudio) << "start" << sampleRate << channels << "startPaused=" << startPaused;

    if (m_session.load(std::memory_order_acquire) != nullptr) {
        qCDebug(dragonsdlAudio) << "start() called while already started stopping old session";
        stop();
    }

    m_totalSamplesWritten.store(0, std::memory_order_relaxed);

    auto *session = new AudioSession;
    session->channels = channels;
    session->sampleRate = sampleRate;

    const SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, sampleRate};

    SDL_AudioStream *stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!stream) {
        qCCritical(dragonsdlAudio) << "SDL_CreateAudioStream failed:" << SDL_GetError();
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }
    session->stream = stream;

    SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (deviceId == 0) {
        qCCritical(dragonsdlAudio) << "SDL_OpenAudioDevice failed:" << SDL_GetError();
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }
    session->deviceId = deviceId;

    if (!SDL_BindAudioStreams(deviceId, &stream, 1)) {
        qCCritical(dragonsdlAudio) << "SDL_BindAudioStreams failed:" << SDL_GetError();
        SDL_CloseAudioDevice(deviceId);
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    restoreVolume(stream);

    qCDebug(dragonsdlAudio) << "registering get callback on stream";
    if (!SDL_SetAudioStreamGetCallback(stream, &DragonAudioOutput::audioStreamCallback, this)) {
        qCCritical(dragonsdlAudio) << "SDL_SetAudioStreamGetCallback FAILED:" << SDL_GetError();
        SDL_CloseAudioDevice(deviceId);
        SDL_DestroyAudioStream(stream);
        delete session;
        Q_EMIT errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    qCDebug(dragonsdlAudio) << "get callback registered successfully";

    if (!startPaused) {
        SDL_ResumeAudioDevice(deviceId);
    }

    m_session.store(session, std::memory_order_release);
    qCDebug(dragonsdlAudio) << "SDL audio device started";
}

void DragonAudioOutput::pause()
{
    if (const auto *session = m_session.load(std::memory_order_acquire); session) {
        if (session->deviceId != 0) {
            SDL_PauseAudioDevice(session->deviceId);
        }
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, 0.0f);
        }
    }
}

void DragonAudioOutput::resume()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->deviceId != 0) {
            SDL_ResumeAudioDevice(session->deviceId);
        }
        restoreVolume(session->stream);
    }
}

void DragonAudioOutput::silence()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->stream) {
            SDL_SetAudioStreamGain(session->stream, 0.0f);
        }
    }
}

void DragonAudioOutput::stop()
{
    qCDebug(dragonsdlAudio) << "stop()";

    auto *oldSession = m_session.exchange(nullptr, std::memory_order_acq_rel);

    if (oldSession) {
        if (oldSession->deviceId != 0) {
            SDL_PauseAudioDevice(oldSession->deviceId);
            SDL_CloseAudioDevice(oldSession->deviceId);
        }
        if (oldSession->stream) {
            qCDebug(dragonsdlAudio) << "stop() destroying stream";
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

    qCDebug(dragonsdlAudio) << "stop() complete";
}

void DragonAudioOutput::reset()
{
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    m_positionOffsetMs.store(0, std::memory_order_relaxed);
}

bool DragonAudioOutput::isQueueReady() const
{
    return m_queueReady.load(std::memory_order_acquire);
}

void DragonAudioOutput::setQueueReady(bool ready)
{
    m_queueReady.store(ready, std::memory_order_release);
}

void DragonAudioOutput::setPositionOffset(int64_t offsetMs, PositionResetMode mode)
{
    m_positionOffsetMs.store(offsetMs, std::memory_order_relaxed);

    if (isPaused()) {
        if (mode == PositionResetMode::Seek || mode == PositionResetMode::NormalTrackChange) {
            auto *pipe = m_audioPipe.load(std::memory_order_acquire);
            if (pipe) {
                pipe->consumer().drain();
            }
            clearStream();
            m_queueReady.store(true, std::memory_order_release);
        }
        m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        return;
    }

    switch (mode) {
    case PositionResetMode::Seek:
    case PositionResetMode::NormalTrackChange:
        m_flushPending.store(true, std::memory_order_release);
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    case PositionResetMode::GaplessTransition:
        m_positionResetPending.store(true, std::memory_order_release);
        break;
    }
}

void DragonAudioOutput::clearStream()
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (session && session->stream) {
        SDL_ClearAudioStream(session->stream);
    }
}

bool DragonAudioOutput::isDeviceOpen() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    return session != nullptr && session->deviceId != 0 && session->stream != nullptr;
}

bool DragonAudioOutput::isPaused() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || session->deviceId == 0) {
        return false;
    }
    return SDL_AudioDevicePaused(session->deviceId);
}

namespace
{

float sliderToLinearGain(float sliderValue)
{
    sliderValue = std::clamp(sliderValue, 0.0f, 1.0f);

    if (sliderValue <= 0.001f) {
        return 0.0f;
    }

    if (sliderValue >= 0.99f) {
        return 1.0f;
    }

    constexpr float LOG100 = 4.60517018599f;
    return -std::log(1.0f - sliderValue) / LOG100;
}

float calculateGain(float volume, bool muted)
{
    return muted ? 0.0f : sliderToLinearGain(volume);
}

}

float DragonAudioOutput::volume() const
{
    return m_volume;
}

void DragonAudioOutput::restoreVolume(SDL_AudioStream *stream)
{
    if (stream) {
        SDL_SetAudioStreamGain(stream, calculateGain(m_volume, m_muted));
    }
}

void DragonAudioOutput::setVolume(float volume)
{
    float clampedVolume = std::clamp(volume, 0.0f, 1.0f);
    if (qAbs(m_volume - clampedVolume) < 0.001f) {
        return;
    }
    m_volume = clampedVolume;

    if (auto *session = m_session.load(std::memory_order_acquire); session && session->stream && !m_muted) {
        restoreVolume(session->stream);
    }
    Q_EMIT volumeChanged();
}

bool DragonAudioOutput::muted() const
{
    return m_muted;
}

void DragonAudioOutput::setMuted(bool muted)
{
    if (m_muted == muted) {
        return;
    }
    m_muted = muted;
    if (auto *session = m_session.load(std::memory_order_acquire); session && session->stream) {
        if (!isPaused()) {
            restoreVolume(session->stream);
        }
    }
    Q_EMIT volumeChanged();
}

void DragonAudioOutput::setStreamName(const QString &name)
{
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, name.toUtf8().constData());
}

int64_t DragonAudioOutput::positionMs() const
{
    auto *session = m_session.load(std::memory_order_acquire);
    if (!session || session->channels <= 0 || session->sampleRate <= 0) {
        return m_positionOffsetMs.load(std::memory_order_relaxed);
    }

    int64_t written = m_totalSamplesWritten.load(std::memory_order_relaxed);

    const int bytesQueued = SDL_GetAudioStreamQueued(session->stream);
    if (bytesQueued > 0) {
        const int64_t samplesQueued = bytesQueued / static_cast<int>(sizeof(float));
        written = std::max(int64_t{0}, written - samplesQueued);
    }

    const int64_t frameCount = written / session->channels;
    return (frameCount * 1000 / session->sampleRate) + m_positionOffsetMs.load(std::memory_order_relaxed);
}

int64_t DragonAudioOutput::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

bool DragonAudioOutput::hasFormat(int sampleRate, int channels) const
{
    auto *session = m_session.load(std::memory_order_acquire);
    return session != nullptr && session->deviceId != 0 && session->sampleRate == sampleRate && session->channels == channels;
}

void SDLCALL DragonAudioOutput::audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int)
{
    auto *self = static_cast<DragonAudioOutput *>(userdata);
    if (!self)
        return;

    thread_local static bool audioThreadNamed = false;
    if (!audioThreadNamed) {
        pthread_setname_np(pthread_self(), "dragon-audio");
        audioThreadNamed = true;
    }
    self->m_activeCallbacks.fetch_add(1, std::memory_order_relaxed);
    struct Guard {
        DragonAudioOutput *self;
        ~Guard()
        {
            self->m_activeCallbacks.fetch_sub(1, std::memory_order_relaxed);
            self->m_callbackDoneCv.notify_one();
        }
    };
    Guard guard{self};

    Q_EMIT self->audioCallbackInvoked();

    const auto *session = self->m_session.load(std::memory_order_acquire);
    if (!session)
        return;

    auto *audioPipe = self->m_audioPipe.load(std::memory_order_acquire);
    if (!audioPipe)
        return;

    if (self->m_positionResetPending.exchange(false, std::memory_order_acq_rel)) {
        self->m_totalSamplesWritten.store(0, std::memory_order_relaxed);
    }

    if (self->m_flushPending.exchange(false, std::memory_order_acq_rel)) {
        audioPipe->consumer().drain();
        self->m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        self->m_queueReady.store(true, std::memory_order_release);
        return;
    }

    if (additional_amount <= 0)
        return;

    const int channels = session->channels;
    const size_t floatsNeeded = (static_cast<size_t>(additional_amount) / sizeof(float)) / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    if (floatsNeeded == 0)
        return;

    size_t totalFloatsRead = 0;

    audioPipe->consumer().readSomeWith(floatsNeeded, [&](std::span<const std::float32_t> b1, std::span<const std::float32_t> b2) {
        totalFloatsRead = b1.size() + b2.size();

        if (!b1.empty()) {
            SDL_PutAudioStreamData(stream, b1.data(), static_cast<int>(b1.size() * sizeof(float)));
        }
        if (!b2.empty()) {
            SDL_PutAudioStreamData(stream, b2.data(), static_cast<int>(b2.size() * sizeof(float)));
        }

        auto *fftPipe = self->m_fftPipe.load(std::memory_order_acquire);
        if (fftPipe && totalFloatsRead > 0) {
            fftPipe->producer().writeSomeWith(totalFloatsRead, [&](std::span<std::float32_t> fb1, std::span<std::float32_t> fb2) {
                size_t srcOffset = 0;
                auto fillDst = [&](std::span<std::float32_t> dst) {
                    for (size_t i = 0; i < dst.size() && srcOffset < totalFloatsRead; ++i, ++srcOffset) {
                        dst[i] = (srcOffset < b1.size()) ? b1[srcOffset] : b2[srcOffset - b1.size()];
                    }
                };
                fillDst(fb1);
                fillDst(fb2);
            });
        }
    });

    if (totalFloatsRead > 0) {
        self->m_totalSamplesWritten.fetch_add(static_cast<int64_t>(totalFloatsRead), std::memory_order_relaxed);
    } else {
        qCDebug(dragonsdlAudio) << "STARVATION additional=" << additional_amount << "floatsNeeded=" << floatsNeeded << "itemsRead=0";
    }
}
