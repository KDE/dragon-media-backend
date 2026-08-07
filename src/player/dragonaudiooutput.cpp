/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"
#include "sink/dragonaudiosink.h"
#include "sink/dragonaudiosinkfactory.h"
#include <DragonMultimedia/dragonaudiooutput.h>
#include <DragonMultimedia/dragonplayer.h>

#include <memory>

class DragonAudioOutputPrivate
{
public:
    explicit DragonAudioOutputPrivate(DragonAudioOutput::Backend requestedBackend)
        : requestedBackend(requestedBackend)
    {
    }

    std::unique_ptr<DragonAudioSink> sink;
    DragonAudioOutput::Backend requestedBackend;
    DragonAudioOutput::Backend selectedBackend = DragonAudioOutput::Backend::Auto;
};

DragonAudioOutput::DragonAudioOutput(Backend requested, DragonPlayer *parent)
    : QObject(parent)
    , d(std::make_unique<DragonAudioOutputPrivate>(requested))
{
    d->sink = createAudioSink(requested, &d->selectedBackend);

    if (d->sink) {
        connect(d->sink.get(), &DragonAudioSink::volumeChanged, this, &DragonAudioOutput::volumeChanged);
    }
}

DragonAudioOutput::~DragonAudioOutput() = default;

DragonAudioSink *DragonAudioOutput::sink() const
{
    return d->sink.get();
}

qreal DragonAudioOutput::volume() const
{
    return d->sink ? d->sink->volume() : 1.0;
}

bool DragonAudioOutput::muted() const
{
    return d->sink ? d->sink->muted() : false;
}

DragonAudioOutput::Backend DragonAudioOutput::backend() const
{
    return d->selectedBackend;
}

void DragonAudioOutput::setVolume(qreal linearGain)
{
    if (d->sink) {
        d->sink->setVolume(static_cast<float>(linearGain));
    }
}

void DragonAudioOutput::setMuted(bool muted)
{
    if (d->sink && d->sink->muted() == muted) {
        return;
    }
    if (d->sink) {
        d->sink->setMuted(muted);
    }
    Q_EMIT mutedChanged(muted);
}
