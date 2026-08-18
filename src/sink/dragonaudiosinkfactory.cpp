/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonaudiosinkfactory.h"
#include "dragonaudiosink.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <algorithm>

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

static std::unique_ptr<DragonAudioSink> tryLoad(const KPluginMetaData &md, DragonAudioOutput::Backend *selectedSinkOut)
{
    auto result = KPluginFactory::instantiatePlugin<DragonAudioSink>(md);
    if (!result) {
        qCWarning(dragonMediaBackendFactory) << "Failed to load audio sink plugin:" << md.pluginId() << "-" << result.errorString;
        return nullptr;
    }
    auto sink = std::unique_ptr<DragonAudioSink>(result.plugin);
    if (!sink->probe()) {
        qCWarning(dragonMediaBackendFactory) << "Audio sink plugin" << md.pluginId() << "failed runtime probe, falling through";
        return nullptr;
    }
    qCDebug(dragonMediaBackendFactory) << "Audio sink plugin" << md.pluginId() << "passed probe, selected";
    assignSelectedSink(selectedSinkOut, md.pluginId());
    return sink;
}

static std::unique_ptr<DragonAudioSink> tryLoadById(const QList<KPluginMetaData> &plugins, const QString &pluginId, DragonAudioOutput::Backend *selectedSinkOut)
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

std::unique_ptr<DragonAudioSink> createAudioSink(DragonAudioOutput::Backend requestedSink, DragonAudioOutput::Backend *selectedSinkOut)
{
    auto plugins = KPluginMetaData::findPlugins(u"DragonMediaBackend/AudioSink"_s);

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(u"Priority"_s, 0) > b.value(u"Priority"_s, 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonMediaBackendFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(u"Priority"_s, 0);
    }

    QString requestedPluginId;
    switch (requestedSink) {
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
    default:
        break;
    }

    if (!requestedPluginId.isEmpty()) {
        if (auto sink = tryLoadById(plugins, requestedPluginId, selectedSinkOut)) {
            return sink;
        }
    }

    const QString envSink = qEnvironmentVariable("DRAGONMULTIMEDIA_AUDIO_SINK");
    if (!envSink.isEmpty()) {
        return tryLoadById(plugins, envSink, selectedSinkOut);
    }

    for (const auto &md : plugins) {
        if (auto sink = tryLoad(md, selectedSinkOut)) {
            return sink;
        }
    }

    return nullptr;
}
