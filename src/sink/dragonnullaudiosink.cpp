/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonnullaudiosink.h"
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

DragonNullAudioSink::DragonNullAudioSink(QObject *parent)
    : DragonAudioSink(parent)
{
    qCWarning(dragonMediaBackendFactory) << "Null audio sink created; audio output will be silent";
    QMetaObject::invokeMethod(
        this,
        [this]() {
            Q_EMIT errorOccurred(nullSinkErrorMessage());
        },
        Qt::QueuedConnection);
}

bool DragonNullAudioSink::probe()
{
    return true;
}

void DragonNullAudioSink::open(int sampleRate, int channels)
{
    setFormat(sampleRate, channels);
    Q_EMIT errorOccurred(nullSinkErrorMessage());
}

void DragonNullAudioSink::close()
{
}

void DragonNullAudioSink::pause()
{
}

void DragonNullAudioSink::resume()
{
}

void DragonNullAudioSink::setGain(float)
{
}

qint64 DragonNullAudioSink::deviceQueuedSamples() const
{
    return 0;
}

bool DragonNullAudioSink::isDeviceOpen() const
{
    // Never open: there is no device. This also disables DragonPlayer's
    // position tracking, which is acceptable for a broken install.
    return false;
}

bool DragonNullAudioSink::isPaused() const
{
    return false;
}

void DragonNullAudioSink::clearStream()
{
}
