/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "dragonaudiosinkfactory.h"
#include "dragonaudiosink.h"

#include <KPluginFactory>
#include <KPluginMetaData>

#include <algorithm>

#include "dragonsdl_factory_logging.h"

using namespace Qt::StringLiterals;

std::unique_ptr<DragonAudioSink> createAudioSink()
{
    auto plugins = KPluginMetaData::findPlugins(QStringLiteral("dragonsdl/audiosink"));

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(QStringLiteral("Priority"), 0) > b.value(QStringLiteral("Priority"), 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonsdlFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(QStringLiteral("Priority"), 0);
    }

    const QString envSink = qEnvironmentVariable("DRAGONSDL_AUDIO_SINK");
    if (!envSink.isEmpty()) {
        auto it = std::ranges::find_if(plugins, [&](const KPluginMetaData &md) {
            qDebug() << "looking at" << md.pluginId() << "with priority" << md.value("Priority"_L1);
            return md.pluginId() == envSink;
        });
        if (it != plugins.end()) {
            if (const auto result = KPluginFactory::instantiatePlugin<DragonAudioSink>(*it)) {
                qCDebug(dragonsdlFactory) << "Successfully loaded requested audio sink plugin:" << it->pluginId();
                return std::unique_ptr<DragonAudioSink>(result.plugin);
            } else {
                qCWarning(dragonsdlFactory) << "Failed to load requested audio sink plugin:" << it->pluginId() << "-" << result.errorString;
            }
        } else {
            qCWarning(dragonsdlFactory) << "Requested audio sink plugin not found:" << envSink;
        }
        return nullptr;
    }

    for (const auto &md : plugins) {
        if (const auto result = KPluginFactory::instantiatePlugin<DragonAudioSink>(md)) {
            qCDebug(dragonsdlFactory) << "Successfully loaded audio sink plugin:" << md.pluginId();
            return std::unique_ptr<DragonAudioSink>(result.plugin);
        } else {
            qCWarning(dragonsdlFactory) << "Failed to load audio sink plugin:" << md.pluginId() << "-" << result.errorString;
        }
    }

    return nullptr;
}
