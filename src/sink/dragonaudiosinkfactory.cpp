/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonaudiosinkfactory.h"
#include "dragonaudiosink.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <algorithm>

#include "dragonmultimedia_factory_logging.h"

using namespace Qt::StringLiterals;

static void assignSelectedSink(DragonPlayer::AudioSink *selectedSinkOut, const QString &pluginId)
{
    if (!selectedSinkOut)
        return;
    if (pluginId == u"dragonpipewireaudiosink"_s) {
        *selectedSinkOut = DragonPlayer::AudioSink::PipeWire;
    } else if (pluginId == u"dragonpulseaudiosink"_s) {
        *selectedSinkOut = DragonPlayer::AudioSink::PulseAudio;
    } else if (pluginId == u"dragonsdlaudiosink"_s) {
        *selectedSinkOut = DragonPlayer::AudioSink::SDL;
    } else {
        *selectedSinkOut = DragonPlayer::AudioSink::Auto;
    }
}

static std::unique_ptr<DragonAudioSink> tryLoad(const KPluginMetaData &md, DragonPlayer::AudioSink *selectedSinkOut)
{
    auto result = KPluginFactory::instantiatePlugin<DragonAudioSink>(md);
    if (!result) {
        qCWarning(dragonMultimediaFactory) << "Failed to load audio sink plugin:" << md.pluginId() << "-" << result.errorString;
        return nullptr;
    }
    auto sink = std::unique_ptr<DragonAudioSink>(result.plugin);
    if (!sink->probe()) {
        qCWarning(dragonMultimediaFactory) << "Audio sink plugin" << md.pluginId() << "failed runtime probe, falling through";
        return nullptr;
    }
    qCDebug(dragonMultimediaFactory) << "Audio sink plugin" << md.pluginId() << "passed probe, selected";
    assignSelectedSink(selectedSinkOut, md.pluginId());
    return sink;
}

std::unique_ptr<DragonAudioSink> createAudioSink(DragonPlayer::AudioSink requestedSink, DragonPlayer::AudioSink *selectedSinkOut)
{
    auto plugins = KPluginMetaData::findPlugins(u"DragonMultimedia/AudioSink"_s);

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(u"Priority"_s, 0) > b.value(u"Priority"_s, 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonMultimediaFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(u"Priority"_s, 0);
    }

    QString requestedPluginId;
    switch (requestedSink) {
    case DragonPlayer::AudioSink::PipeWire:
        requestedPluginId = u"dragonpipewireaudiosink"_s;
        break;
    case DragonPlayer::AudioSink::PulseAudio:
        requestedPluginId = u"dragonpulseaudiosink"_s;
        break;
    case DragonPlayer::AudioSink::SDL:
        requestedPluginId = u"dragonsdlaudiosink"_s;
        break;
    case DragonPlayer::AudioSink::Auto:
    default:
        break;
    }

    if (!requestedPluginId.isEmpty()) {
        auto it = std::ranges::find_if(plugins, [&](const KPluginMetaData &md) {
            return md.pluginId() == requestedPluginId;
        });
        if (it != plugins.end()) {
            if (auto sink = tryLoad(*it, selectedSinkOut)) {
                return sink;
            }
        } else {
            qCWarning(dragonMultimediaFactory) << "Requested audio sink plugin not found:" << requestedPluginId;
        }
    }

    const QString envSink = qEnvironmentVariable("DRAGONMULTIMEDIA_AUDIO_SINK");
    if (!envSink.isEmpty()) {
        auto it = std::ranges::find_if(plugins, [&](const KPluginMetaData &md) {
            return md.pluginId() == envSink;
        });
        if (it != plugins.end()) {
            if (auto sink = tryLoad(*it, selectedSinkOut)) {
                return sink;
            }
        } else {
            qCWarning(dragonMultimediaFactory) << "Requested audio sink plugin not found:" << envSink;
        }
        return nullptr;
    }

    for (const auto &md : plugins) {
        if (auto sink = tryLoad(md, selectedSinkOut)) {
            return sink;
        }
    }

    return nullptr;
}
