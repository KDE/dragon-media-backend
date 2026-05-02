/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragonaudiooutput.h>

#include <LockFreeSpscQueue.h>

#include <QDebug>
#include <QGuiApplication>
#include <QIcon>

#include <chrono>

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

DragonAudioOutput::DragonAudioOutput(QObject *parent)
    : QObject(parent)
{
    const QString iconName = QGuiApplication::windowIcon().name();
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_APP_ICON_NAME, iconName.isEmpty() ? "dragon-sdl" : iconName.toUtf8().constData());
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_ROLE, "music");

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        qCritical() << "SDL_Init(SDL_INIT_AUDIO) failed:" << SDL_GetError();
        emit errorOccurred(QString::fromUtf8(SDL_GetError()));
    }
}

DragonAudioOutput::~DragonAudioOutput()
{
    stop();
    SDL_Quit();
}

void DragonAudioOutput::setQueue(LockFreeSpscQueue<float> *queue)
{
    m_audioQueue = queue;
}

void DragonAudioOutput::start(int sampleRate, int channels)
{
    qDebug() << "DragonAudioOutput::start" << sampleRate << channels;

    m_channels = channels;
    m_sampleRate = sampleRate;
    m_totalSamplesWritten = 0;

    const SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, sampleRate};

    m_stream = SDL_CreateAudioStream(&spec, nullptr);
    if (!m_stream) {
        qCritical() << "SDL_CreateAudioStream failed:" << SDL_GetError();
        emit errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    m_deviceId = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (m_deviceId == 0) {
        qCritical() << "SDL_OpenAudioDevice failed:" << SDL_GetError();
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        emit errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    if (!SDL_BindAudioStreams(m_deviceId, &m_stream, 1)) {
        qCritical() << "SDL_BindAudioStreams failed:" << SDL_GetError();
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
        emit errorOccurred(QString::fromUtf8(SDL_GetError()));
        return;
    }

    SDL_SetAudioStreamGain(m_stream, m_muted ? 0.0f : m_volume);

    m_pumpStopSource = std::stop_source{};
    m_pumpThread = std::jthread([this](std::stop_token st) {
        pumpLoop(st);
    });

    SDL_ResumeAudioDevice(m_deviceId);
    qDebug() << "SDL audio device started";
}

void DragonAudioOutput::stop()
{
    if (m_pumpStopSource.stop_possible()) {
        m_pumpStopSource.request_stop();
    }
    m_pumpThread = std::jthread{};

    if (m_deviceId != 0) {
        SDL_PauseAudioDevice(m_deviceId);
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
    }
    if (m_stream) {
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
}

void DragonAudioOutput::reset()
{
    m_totalSamplesWritten = 0;
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
    if (m_stream && !m_muted) {
        SDL_SetAudioStreamGain(m_stream, linearGain);
    }
    emit volumeChanged();
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
    if (m_stream) {
        SDL_SetAudioStreamGain(m_stream, muted ? 0.0f : m_volume);
    }
    emit volumeChanged();
}

void DragonAudioOutput::setStreamName(const QString &name)
{
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, name.toUtf8().constData());
}

int64_t DragonAudioOutput::positionMs() const
{
    int64_t written = m_totalSamplesWritten.load(std::memory_order_relaxed);

    if (m_stream && m_channels > 0) {
        const int bytesQueued = SDL_GetAudioStreamQueued(m_stream);
        if (bytesQueued > 0) {
            const int64_t samplesQueued = bytesQueued / static_cast<int>(sizeof(float));
            written = std::max(int64_t{0}, written - samplesQueued);
        }
    }

    const int64_t frameCount = written / m_channels;
    return frameCount * 1000 / m_sampleRate;
}

int64_t DragonAudioOutput::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

void DragonAudioOutput::pumpLoop(std::stop_token st)
{
    static constexpr size_t chunkSize = 4096;

    while (!st.stop_requested()) {
        if (!m_audioQueue) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        if (!m_stream) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        auto scope = m_audioQueue->prepare_read(chunkSize);
        const size_t itemsRead = scope.get_items_read();

        if (itemsRead > 0) {
            auto block1 = scope.get_block1();
            if (!block1.empty()) {
                SDL_PutAudioStreamData(m_stream, block1.data(), static_cast<int>(block1.size() * sizeof(float)));
            }
            auto block2 = scope.get_block2();
            if (!block2.empty()) {
                SDL_PutAudioStreamData(m_stream, block2.data(), static_cast<int>(block2.size() * sizeof(float)));
            }

            m_totalSamplesWritten.fetch_add(static_cast<int64_t>(itemsRead), std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
}