/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonplayer_p.h"
#include "sink/playbackpipeline.h"
#include "sink/playbackpipelinefactory.h"
#include <dragonaudiooutput.h>
#include <dragonplayer.h>

#include <memory>

class DragonAudioOutputPrivate
{
public:
    explicit DragonAudioOutputPrivate(DragonAudioOutput::Backend requestedBackend)
        : requestedBackend(requestedBackend)
    {
    }

    std::unique_ptr<PlaybackPipeline> pipeline;
    DragonAudioOutput::Backend requestedBackend;
    DragonAudioOutput::Backend selectedBackend = DragonAudioOutput::Backend::Auto;
};

DragonAudioOutput::DragonAudioOutput(Backend requested, DragonPlayer *parent)
    : QObject(parent)
    , d(std::make_unique<DragonAudioOutputPrivate>(requested))
{
    d->pipeline = createPlaybackPipeline(requested, &d->selectedBackend);

    if (d->pipeline) {
        connect(d->pipeline.get(), &PlaybackPipeline::volumeChanged, this, [this]() {
            Q_EMIT volumeChanged(volume());
        });
        connect(d->pipeline.get(), &PlaybackPipeline::mutedChanged, this, &DragonAudioOutput::mutedChanged);
    }
}

DragonAudioOutput::~DragonAudioOutput() = default;

PlaybackPipeline *DragonAudioOutput::playbackPipeline() const
{
    return d->pipeline.get();
}

qreal DragonAudioOutput::volume() const
{
    return d->pipeline ? d->pipeline->volume() : 1.0;
}

bool DragonAudioOutput::isMuted() const
{
    return d->pipeline ? d->pipeline->muted() : false;
}

bool DragonAudioOutput::muted() const
{
    return isMuted();
}

DragonAudioOutput::Backend DragonAudioOutput::backend() const
{
    return d->selectedBackend;
}

void DragonAudioOutput::setVolume(qreal volume)
{
    if (d->pipeline) {
        d->pipeline->setVolume(static_cast<float>(volume));
    }
}

void DragonAudioOutput::setMuted(bool muted)
{
    if (d->pipeline) {
        d->pipeline->setMuted(muted);
    }
}
