/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "audiosink.h"
#include "mixer/mixer.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>

AudioSink::AudioSink(QObject *parent)
    : QObject(parent)
{
}

AudioSink::~AudioSink() = default;

void AudioSink::setMuted(bool muted)
{
    Q_UNUSED(muted);
}

void AudioSink::setStreamName(const QString &name)
{
    m_streamName = name;
}

QString AudioSink::defaultStreamName()
{
    const QString appName = QGuiApplication::applicationDisplayName();
    return appName.isEmpty() ? QStringLiteral("DragonMediaBackend") : appName;
}

QString AudioSink::resolvedStreamName() const
{
    return m_streamName.isEmpty() ? defaultStreamName() : m_streamName;
}

QString AudioSink::applicationIconName()
{
    QString name = QGuiApplication::desktopFileName();
    if (name.isEmpty() && qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        name = QGuiApplication::windowIcon().name();
    }
    return name;
}

std::size_t AudioSink::preferredCallbackSamples(int sampleRate, int channels) const
{
    Q_UNUSED(sampleRate);
    Q_UNUSED(channels);
    return 0;
}

void AudioSink::setMixer(Mixer *mixer)
{
    m_mixer = mixer;
}

Mixer *AudioSink::mixer() const
{
    return m_mixer;
}
