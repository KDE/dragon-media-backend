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

std::unique_ptr<DragonAudioSink> createAudioSink(DragonPlayer::AudioSink requestedSink, DragonPlayer::AudioSink *selectedSinkOut)
{
    auto plugins = KPluginMetaData::findPlugins(QStringLiteral("DragonMultimedia/audiosink"));

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(QStringLiteral("Priority"), 0) > b.value(QStringLiteral("Priority"), 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonMultimediaFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(QStringLiteral("Priority"), 0);
    }

    auto assignSelectedSink = [selectedSinkOut](const QString &pluginId) {
        if (!selectedSinkOut)
            return;
        if (pluginId == QStringLiteral("dragonpipewireaudiosink")) {
            *selectedSinkOut = DragonPlayer::AudioSink::PipeWire;
        } else if (pluginId == QStringLiteral("dragonpulseaudiosink")) {
            *selectedSinkOut = DragonPlayer::AudioSink::PulseAudio;
        } else if (pluginId == QStringLiteral("dragonsdlaudiosink")) {
            *selectedSinkOut = DragonPlayer::AudioSink::SDL;
        } else {
            *selectedSinkOut = DragonPlayer::AudioSink::Auto;
        }
    };

    auto tryLoad = [&](const KPluginMetaData &md) -> std::unique_ptr<DragonAudioSink> {
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
        assignSelectedSink(md.pluginId());
        return sink;
    };

    QString requestedPluginId;
    switch (requestedSink) {
    case DragonPlayer::AudioSink::PipeWire:
        requestedPluginId = QStringLiteral("dragonpipewireaudiosink");
        break;
    case DragonPlayer::AudioSink::PulseAudio:
        requestedPluginId = QStringLiteral("dragonpulseaudiosink");
        break;
    case DragonPlayer::AudioSink::SDL:
        requestedPluginId = QStringLiteral("dragonsdlaudiosink");
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
            if (auto sink = tryLoad(*it)) {
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
            if (auto sink = tryLoad(*it)) {
                return sink;
            }
        } else {
            qCWarning(dragonMultimediaFactory) << "Requested audio sink plugin not found:" << envSink;
        }
        return nullptr;
    }

    for (const auto &md : plugins) {
        if (auto sink = tryLoad(md)) {
            return sink;
        }
    }

    return nullptr;
}
