/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiosinkfactory.h"
#include "dragonaudiosink.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <algorithm>

#include "dragonmultimedia_factory_logging.h"

using namespace Qt::StringLiterals;

std::unique_ptr<DragonAudioSink> createAudioSink()
{
    auto plugins = KPluginMetaData::findPlugins(QStringLiteral("DragonMultimedia/audiosink"));

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(QStringLiteral("Priority"), 0) > b.value(QStringLiteral("Priority"), 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonMultimediaFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(QStringLiteral("Priority"), 0);
    }

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
        return sink;
    };

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
