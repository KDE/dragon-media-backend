/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonnullaudiosink.h"
#include "dragonmultimedia_factory_logging.h"

DragonNullAudioSink::DragonNullAudioSink(QObject *parent)
    : DragonAudioSink(parent)
{
    qCWarning(dragonMultimediaFactory) << "Null audio sink created audio output will be silent";
}

bool DragonNullAudioSink::probe()
{
    return true;
}

void DragonNullAudioSink::open(int sampleRate, int channels)
{
    setFormat(sampleRate, channels);
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

int64_t DragonNullAudioSink::deviceQueuedSamples() const
{
    return 0;
}

bool DragonNullAudioSink::isDeviceOpen() const
{
    // Report "open" so position tracking and format checks don't bail.
    return currentSampleRate() > 0;
}

bool DragonNullAudioSink::isPaused() const
{
    return false;
}

void DragonNullAudioSink::clearStream()
{
}
