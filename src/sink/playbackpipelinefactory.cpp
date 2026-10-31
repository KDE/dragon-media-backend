/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "playbackpipelinefactory.h"
#include "audiosink.h"
#include "mixer/mixer.h"
#include "nullaudiosink.h"
#include "playbackpipeline.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <algorithm>
#include <memory>
#include <utility>

#include "dragonmediabackend_factory_logging.h"

using namespace Qt::StringLiterals;

static void assignSelectedSink(DragonAudioOutput::Backend *selectedSinkOut, const QString &pluginId)
{
    if (!selectedSinkOut)
        return;
    if (pluginId == u"dragonpipewireaudiosink"_s) {
        *selectedSinkOut = DragonAudioOutput::Backend::PipeWire;
    } else if (pluginId == u"dragonpulseaudiosink"_s) {
        *selectedSinkOut = DragonAudioOutput::Backend::PulseAudio;
    } else if (pluginId == u"dragonsdlaudiosink"_s) {
        *selectedSinkOut = DragonAudioOutput::Backend::SDL;
    } else {
        *selectedSinkOut = DragonAudioOutput::Backend::Auto;
    }
}

// Load the given plugin
static std::unique_ptr<PlaybackPipeline> tryLoad(const KPluginMetaData &md, DragonAudioOutput::Backend *selectedSinkOut)
{
    auto result = KPluginFactory::instantiatePlugin<AudioSink>(md);
    if (!result) {
        qCWarning(dragonMediaBackendFactory) << "Failed to load audio sink plugin:" << md.pluginId() << "-" << result.errorString;
        return nullptr;
    }
    auto sink = std::unique_ptr<AudioSink>(result.plugin);
    if (!sink->probe()) {
        qCWarning(dragonMediaBackendFactory) << "Audio sink plugin" << md.pluginId() << "failed runtime probe, falling through";
        return nullptr;
    }
    qCDebug(dragonMediaBackendFactory) << "Audio sink plugin" << md.pluginId() << "passed probe, selected";
    assignSelectedSink(selectedSinkOut, md.pluginId());
    return std::make_unique<PlaybackPipeline>(std::move(sink));
}

static std::unique_ptr<PlaybackPipeline>
tryLoadById(const QList<KPluginMetaData> &plugins, const QString &pluginId, DragonAudioOutput::Backend *selectedSinkOut)
{
    auto it = std::ranges::find_if(plugins, [&](const KPluginMetaData &md) {
        return md.pluginId() == pluginId;
    });
    if (it != plugins.end()) {
        return tryLoad(*it, selectedSinkOut);
    }
    qCWarning(dragonMediaBackendFactory) << "Requested audio sink plugin not found:" << pluginId;
    return nullptr;
}

static std::unique_ptr<PlaybackPipeline> makeNullPipeline(DragonAudioOutput::Backend *selectedSinkOut)
{
    if (selectedSinkOut) {
        *selectedSinkOut = DragonAudioOutput::Backend::Null;
    }
    return std::make_unique<PlaybackPipeline>(std::make_unique<NullAudioSink>());
}

std::unique_ptr<PlaybackPipeline> createPlaybackPipeline(DragonAudioOutput::Backend requestedSink, DragonAudioOutput::Backend *selectedSinkOut)
{
    if (requestedSink == DragonAudioOutput::Backend::Null) {
        return makeNullPipeline(selectedSinkOut);
    }

    QString pluginDir = u"DragonMediaBackend/AudioSink"_s;
    const QString envPluginDir = qEnvironmentVariable("DRAGON_AUDIO_SINK_PLUGIN_DIR");
    if (!envPluginDir.isEmpty()) {
        pluginDir = envPluginDir;
        qCDebug(dragonMediaBackendFactory) << "Audio sink plugin search directory overridden via DRAGON_AUDIO_SINK_PLUGIN_DIR:" << pluginDir;
    }
    auto plugins = KPluginMetaData::findPlugins(pluginDir);

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(u"Priority"_s, 0) > b.value(u"Priority"_s, 0);
    });

    for (const auto &md : std::as_const(plugins)) {
        qCDebug(dragonMediaBackendFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(u"Priority"_s, 0);
    }

    QString requestedPluginId;
    switch (requestedSink) {
    case DragonAudioOutput::Backend::Null:
        return makeNullPipeline(selectedSinkOut);
    case DragonAudioOutput::Backend::PipeWire:
        requestedPluginId = u"dragonpipewireaudiosink"_s;
        break;
    case DragonAudioOutput::Backend::PulseAudio:
        requestedPluginId = u"dragonpulseaudiosink"_s;
        break;
    case DragonAudioOutput::Backend::SDL:
        requestedPluginId = u"dragonsdlaudiosink"_s;
        break;
    case DragonAudioOutput::Backend::Auto:
        break;
    }

    if (!requestedPluginId.isEmpty()) {
        if (auto pipeline = tryLoadById(plugins, requestedPluginId, selectedSinkOut)) {
            return pipeline;
        }
    }

    const QString envSink = qEnvironmentVariable("DRAGON_AUDIO_SINK");
    if (!envSink.isEmpty()) {
        if (envSink == u"dragonnullaudiosink"_s) {
            return makeNullPipeline(selectedSinkOut);
        }
        if (auto pipeline = tryLoadById(plugins, envSink, selectedSinkOut)) {
            return pipeline;
        }
    }

    for (const auto &md : std::as_const(plugins)) {
        if (auto pipeline = tryLoad(md, selectedSinkOut)) {
            return pipeline;
        }
    }

    qCWarning(dragonMediaBackendFactory) << "No audio sink plugin found or loaded, falling back to null audio sink";
    return makeNullPipeline(selectedSinkOut);
}
