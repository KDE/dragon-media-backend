/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include <dragonsdl/dragonaudiooutput.h>

#include <LockFreeSpscQueue.h>

#include <QDebug>
#include <QGuiApplication>
#include <QIcon>

using namespace Qt::StringLiterals;

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

    qDebug() << "AUDIO_OUT: registering get callback on stream";
    if (!SDL_SetAudioStreamGetCallback(m_stream, &DragonAudioOutput::audioStreamCallback, this)) {
        qCritical() << "AUDIO_OUT: SDL_SetAudioStreamGetCallback FAILED:" << SDL_GetError();
        emit errorOccurred(QString::fromUtf8(SDL_GetError()));
    } else {
        qDebug() << "AUDIO_OUT: get callback registered successfully";
    }

    SDL_ResumeAudioDevice(m_deviceId);
    qDebug() << "SDL audio device started";
}

void DragonAudioOutput::pause()
{
    if (m_deviceId != 0) {
        SDL_PauseAudioDevice(m_deviceId);
    }
}

void DragonAudioOutput::resume()
{
    if (m_deviceId != 0) {
        SDL_ResumeAudioDevice(m_deviceId);
    }
}

void DragonAudioOutput::stop()
{
    qDebug() << "AUDIO_OUT: stop() pausing device=" << m_deviceId;
    if (m_deviceId != 0) {
        SDL_PauseAudioDevice(m_deviceId);
        SDL_CloseAudioDevice(m_deviceId);
        m_deviceId = 0;
    }
    if (m_stream) {
        qDebug() << "AUDIO_OUT: stop() destroying stream";
        SDL_DestroyAudioStream(m_stream);
        m_stream = nullptr;
    }
    qDebug() << "AUDIO_OUT: stop() complete";
}

void DragonAudioOutput::reset()
{
    m_totalSamplesWritten = 0;
    m_positionOffsetMs.store(0, std::memory_order_relaxed);
}

void DragonAudioOutput::setPositionOffset(int64_t offsetMs)
{
    m_positionOffsetMs.store(offsetMs, std::memory_order_relaxed);
    m_totalSamplesWritten.store(0, std::memory_order_relaxed);
}

bool DragonAudioOutput::isDeviceOpen() const
{
    return m_deviceId != 0 && m_stream != nullptr;
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
    return (frameCount * 1000 / m_sampleRate) + m_positionOffsetMs.load(std::memory_order_relaxed);
}

int64_t DragonAudioOutput::totalSamplesWritten() const
{
    return m_totalSamplesWritten.load(std::memory_order_relaxed);
}

void SDLCALL DragonAudioOutput::audioStreamCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int)
{
    auto *self = static_cast<DragonAudioOutput *>(userdata);
    if (!self || !self->m_audioQueue) {
        qDebug() << "AUDIO_CB: no self or no queue";
        return;
    }
    if (additional_amount <= 0) {
        qDebug() << "AUDIO_CB: additional_amount=" << additional_amount << "≤ 0, skipping";
        return;
    }

    const size_t queueReady = self->m_audioQueue->get_num_items_ready();

    const size_t floatsNeeded =
        (static_cast<size_t>(additional_amount) / sizeof(float)) / static_cast<size_t>(self->m_channels) * static_cast<size_t>(self->m_channels);
    if (floatsNeeded == 0) {
        qDebug() << "AUDIO_CB: additional=" << additional_amount << "queueReady=" << queueReady << "→ floatsNeeded=0 (frame-align), skipping";
        return;
    }

    auto scope = self->m_audioQueue->prepare_read(floatsNeeded);
    const size_t itemsRead = scope.get_items_read();

    if (itemsRead == 0) {
        qDebug() << "AUDIO_CB: STARVATION additional=" << additional_amount << "queueReady=" << queueReady << "floatsNeeded=" << floatsNeeded
                 << "itemsRead=0";
        return;
    }

    auto block1 = scope.get_block1();
    auto block2 = scope.get_block2();
    int bytesPushed = 0;
    if (!block1.empty()) {
        SDL_PutAudioStreamData(stream, block1.data(), static_cast<int>(block1.size() * sizeof(float)));
        bytesPushed += static_cast<int>(block1.size() * sizeof(float));
    }
    if (!block2.empty()) {
        SDL_PutAudioStreamData(stream, block2.data(), static_cast<int>(block2.size() * sizeof(float)));
        bytesPushed += static_cast<int>(block2.size() * sizeof(float));
    }

    self->m_totalSamplesWritten.fetch_add(static_cast<int64_t>(itemsRead), std::memory_order_relaxed);

    qDebug() << "AUDIO_CB: additional=" << additional_amount << "queueReady=" << queueReady << "floatsNeeded=" << floatsNeeded << "itemsRead=" << itemsRead
             << "block1=" << block1.size() << "block2=" << block2.size() << "bytesPushed=" << bytesPushed
             << "totalWritten=" << self->m_totalSamplesWritten.load(std::memory_order_relaxed);
}