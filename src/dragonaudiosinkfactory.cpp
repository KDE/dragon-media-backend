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

std::unique_ptr<DragonAudioSink> createAudioSink()
{
    auto plugins = KPluginMetaData::findPlugins(QStringLiteral("dragonsdl/audiosink"));

    std::ranges::sort(plugins, [](const KPluginMetaData &a, const KPluginMetaData &b) {
        return a.value(QStringLiteral("Priority"), 0) > b.value(QStringLiteral("Priority"), 0);
    });

    for (const auto &md : plugins) {
        qCDebug(dragonsdlFactory) << "Found audio sink plugin:" << md.pluginId() << "with priority:" << md.value(QStringLiteral("Priority"), 0);
    }

    for (const auto &md : plugins) {
        auto result = KPluginFactory::instantiatePlugin<DragonAudioSink>(md);
        if (result) {
            qCDebug(dragonsdlFactory) << "Successfully loaded audio sink plugin:" << md.pluginId();
            return std::unique_ptr<DragonAudioSink>(result.plugin);
        } else {
            qCWarning(dragonsdlFactory) << "Failed to load audio sink plugin:" << md.pluginId() << "-" << result.errorString;
        }
    }

    return nullptr;
}
