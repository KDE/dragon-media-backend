/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "nullaudiosink.h"
#include "dragonmediabackend_factory_logging.h"

#include <KLocalizedString>
#include <QMetaObject>

namespace
{
QString nullSinkErrorMessage()
{
    return i18n("Could not find any audio sink plugins.");
}
}

NullAudioSink::NullAudioSink(QObject *parent)
    : AudioSink(parent)
{
    qCWarning(dragonMediaBackendFactory) << "Null audio sink created; audio output will be silent";
    QMetaObject::invokeMethod(
        this,
        [this]() {
            Q_EMIT errorOccurred(nullSinkErrorMessage());
        },
        Qt::QueuedConnection);
}

bool NullAudioSink::probe()
{
    return true;
}

void NullAudioSink::open(int sampleRate, int channels)
{
    // Report the broken install on every open attempt; DragonPlayer's generic
    // open-failure handling sees isDeviceOpen() == false and refuses playback.
    Q_EMIT errorOccurred(nullSinkErrorMessage());
    setFormat(sampleRate, channels);
}

void NullAudioSink::close()
{
}

void NullAudioSink::pause()
{
}

void NullAudioSink::resume()
{
}

void NullAudioSink::setGain(float)
{
}

qint64 NullAudioSink::deviceQueuedSamples() const
{
    return 0;
}

bool NullAudioSink::isDeviceOpen() const
{
    // Never open: there is no device, and open() always fails by contract.
    // The player refuses playback-state transitions to sinks that are not
    // open after an open() attempt, so playback never starts and position
    // tracking never begins.
    return false;
}

bool NullAudioSink::isPaused() const
{
    return false;
}

void NullAudioSink::clearStream()
{
}
