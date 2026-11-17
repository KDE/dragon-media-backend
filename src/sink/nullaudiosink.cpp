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
QString nullDeviceErrorMessage()
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
            Q_EMIT errorOccurred(nullDeviceErrorMessage());
        },
        Qt::QueuedConnection);
}

bool NullAudioSink::probe()
{
    return true;
}

std::expected<std::unique_ptr<AudioSinkStream>, QString>
NullAudioSink::createStream(int sampleRate, int channels, AudioSinkStreamRenderer &renderer, StreamVolumeInit volumeInit)
{
    Q_UNUSED(sampleRate);
    Q_UNUSED(channels);
    Q_UNUSED(renderer);
    Q_UNUSED(volumeInit);
    // Report the broken install on every stream-creation attempt.
    return std::unexpected(nullDeviceErrorMessage());
}
