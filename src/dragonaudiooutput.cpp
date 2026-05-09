/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonaudiooutput.h>
#include <stdfloat>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>

#include <LockFreeSpscQueue.h>

#include "dragonsdl_audio_logging.h"
#include <QGuiApplication>
#include <QIcon>
#include <QString>

#include <algorithm>
#include <chrono>
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

void DragonAudioOutput::setQueue(LockFreeSpscQueue<std::float32_t> *queue)
{
    m_audioQueue.store(queue, std::memory_order_release);
}

void DragonAudioOutput::setFftQueue(LockFreeSpscQueue<std::float32_t> *queue)
{
    m_fftQueue.store(queue, std::memory_order_release);
}

void DragonAudioOutput::start(int sampleRate, int channels)
{
    qCDebug(dragonsdlAudio) << "start" << sampleRate << channels;

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

    SDL_SetAudioStreamGain(stream, m_muted ? 0.0f : m_volume);

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

    SDL_ResumeAudioDevice(deviceId);

    m_session.store(session, std::memory_order_release);
    qCDebug(dragonsdlAudio) << "SDL audio device started";
}

void DragonAudioOutput::pause()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->deviceId != 0) {
            SDL_PauseAudioDevice(session->deviceId);
        }
    }
}

void DragonAudioOutput::resume()
{
    if (auto *session = m_session.load(std::memory_order_acquire)) {
        if (session->deviceId != 0) {
            SDL_ResumeAudioDevice(session->deviceId);
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

void DragonAudioOutput::setPositionOffset(int64_t offsetMs)
{
    m_positionOffsetMs.store(offsetMs, std::memory_order_relaxed);
    m_flushPending.store(true, std::memory_order_release);
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

float DragonAudioOutput::volume() const
{
    return m_volume;
}

void DragonAudioOutput::setVolume(float linearGain)
{
    if (qAbs(m_volume - linearGain) < 0.001f) {
        return;
    }
    m_volume = linearGain;
    if (auto *session = m_session.load(std::memory_order_acquire); session && session->stream && !m_muted) {
        SDL_SetAudioStreamGain(session->stream, linearGain);
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
        SDL_SetAudioStreamGain(session->stream, muted ? 0.0f : m_volume);
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
    if (!self) {
        return;
    }

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
    if (!session) {
        return;
    }

    auto *queue = self->m_audioQueue.load(std::memory_order_acquire);
    if (!queue) {
        return;
    }

    if (self->m_flushPending.exchange(false, std::memory_order_acq_rel)) {
        const size_t ready = queue->get_num_items_ready();
        if (ready > 0) {
            auto drain = queue->prepare_read(ready);
        }
        self->m_totalSamplesWritten.store(0, std::memory_order_relaxed);
        return;
    }

    if (additional_amount <= 0) {
        return;
    }

    const size_t queueReady = queue->get_num_items_ready();

    auto channels = session->channels;
    const size_t floatsNeeded = (static_cast<size_t>(additional_amount) / sizeof(float)) / static_cast<size_t>(channels) * static_cast<size_t>(channels);
    if (floatsNeeded == 0) {
        return;
    }

    auto scope = queue->prepare_read(floatsNeeded);
    const size_t itemsRead = scope.get_items_read();

    if (itemsRead == 0) {
        qCDebug(dragonsdlAudio) << "STARVATION additional=" << additional_amount << "queueReady=" << queueReady << "floatsNeeded=" << floatsNeeded
                                << "itemsRead=0";
        return;
    }

    auto block1 = scope.get_block1();
    auto block2 = scope.get_block2();
    if (!block1.empty()) {
        SDL_PutAudioStreamData(stream, block1.data(), static_cast<int>(block1.size() * sizeof(float)));
    }
    if (!block2.empty()) {
        SDL_PutAudioStreamData(stream, block2.data(), static_cast<int>(block2.size() * sizeof(float)));
    }

    if (auto *fftQueue = self->m_fftQueue.load(std::memory_order_acquire)) {
        const size_t totalSamples = block1.size() + block2.size();
        const size_t frameCount = totalSamples / channels;

        auto getSrc = [&](size_t idx) -> std::float32_t {
            return idx < block1.size() ? block1[idx] : block2[idx - block1.size()];
        };

        [[maybe_unused]] const size_t fftWritten = fftQueue->try_write(frameCount, [&](std::span<std::float32_t> fb1, std::span<std::float32_t> fb2) {
            const size_t fftWriteCapacity = fb1.size() + fb2.size();
            const size_t framesToWrite = std::min(frameCount, fftWriteCapacity);

            const auto computeMono = [&](const size_t frame) -> float {
                const auto base = frame * channels;
                auto channelSamples = std::views::iota(0, channels) | std::views::transform([&](const int c) {
                                          return getSrc(base + c);
                                      });
                return std::ranges::fold_left(channelSamples, 0.0f, std::plus{}) / static_cast<float>(channels);
            };

            const size_t fb1Count = std::min(framesToWrite, fb1.size());

            for (size_t i = 0; i < fb1Count; ++i) {
                fb1[i] = computeMono(i);
            }
            for (size_t i = fb1Count; i < framesToWrite; ++i) {
                fb2[i - fb1Count] = computeMono(i);
            }

            if (fftWriteCapacity < frameCount) {
                qCDebug(dragonsdlAudio) << "Not enough capacity for all frames, skipping" << frameCount - fftWriteCapacity << "frames";
            }
        });
    }

    self->m_fftWaitCv.notify_one();

    self->m_totalSamplesWritten.fetch_add(static_cast<int64_t>(itemsRead), std::memory_order_relaxed);
}