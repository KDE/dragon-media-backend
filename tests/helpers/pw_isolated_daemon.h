/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 *
 * Spawns an isolated PipeWire daemon with a null-audio-sink for testing.
 * No system daemon, no session manager, no hardware required.
 */

#pragma once

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <chrono>
#include <memory>
#include <thread>

using namespace Qt::StringLiterals;

static QString findSpaPluginDir()
{
#ifdef SPA_PLUGIN_DIR
    return QStringLiteral(SPA_PLUGIN_DIR);
#else
    for (const char *candidate : {"/usr/lib64/spa-0.2", "/usr/lib/x86_64-linux-gnu/spa-0.2", "/usr/lib/spa-0.2", "/usr/local/lib/spa-0.2"}) {
        if (QFileInfo::exists(QString::fromLatin1(candidate)))
            return QString::fromLatin1(candidate);
    }
    return {};
#endif
}

static QString findPipeWireModuleDir()
{
#ifdef PIPEWIRE_MODULE_DIR
    return QStringLiteral(PIPEWIRE_MODULE_DIR);
#else
    for (const char *candidate :
         {"/usr/lib64/pipewire-0.3", "/usr/lib/x86_64-linux-gnu/pipewire-0.3", "/usr/lib/pipewire-0.3", "/usr/local/lib/pipewire-0.3"}) {
        if (QFileInfo::exists(QString::fromLatin1(candidate)))
            return QString::fromLatin1(candidate);
    }
    return {};
#endif
}

class PwIsolatedDaemon
{
public:
    PwIsolatedDaemon() = default;

    ~PwIsolatedDaemon()
    {
        stop();
    }

    PwIsolatedDaemon(const PwIsolatedDaemon &) = delete;
    PwIsolatedDaemon &operator=(const PwIsolatedDaemon &) = delete;

    QString socketPath() const
    {
        return m_socketPath;
    }

    bool start()
    {
        if (!m_configDir.isValid()) {
            qWarning() << "PwIsolatedDaemon: failed to create temp dir";
            return false;
        }

        m_socketPath = m_configDir.filePath(u"pipewire-0"_s);

        QString configPath = writeConfig();
        if (configPath.isEmpty()) {
            return false;
        }

        m_process = std::make_unique<QProcess>();
        m_process->setStandardOutputFile(QProcess::nullDevice());
        m_process->setStandardErrorFile(QProcess::nullDevice());

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString spaDir = findSpaPluginDir();
        QString pwDir = findPipeWireModuleDir();
        if (!spaDir.isEmpty()) {
            env.insert(u"SPA_PLUGIN_DIR"_s, spaDir);
        }
        if (!pwDir.isEmpty()) {
            env.insert(u"PIPEWIRE_MODULE_DIR"_s, pwDir);
        }
        env.insert(u"PIPEWIRE_CONFIG_DIR"_s, m_configDir.path());
        env.insert(u"PIPEWIRE_RUNTIME_DIR"_s, m_configDir.path());
        m_process->setProcessEnvironment(env);

        QStringList args;
        args << u"-c"_s << configPath;
        m_process->start(u"pipewire"_s, args);

        if (!m_process->waitForStarted(5000)) {
            qWarning() << "PwIsolatedDaemon: pipewire failed to start:" << m_process->errorString();
            m_process.reset();
            return false;
        }

        if (!waitForSocket(5000)) {
            qWarning() << "PwIsolatedDaemon: socket did not appear";
            stop();
            return false;
        }

        return true;
    }

    void stop()
    {
        if (m_process) {
            m_process->terminate();
            if (!m_process->waitForFinished(3000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
            m_process.reset();
        }
    }

    bool waitForSocket(int timeoutMs)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (QFileInfo::exists(m_socketPath)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }

private:
    QString writeConfig()
    {
        QString configPath = m_configDir.filePath(u"pipewire.conf"_s);
        QFile file(configPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qWarning() << "PwIsolatedDaemon: cannot write config";
            return {};
        }

        QByteArray config = R"pwcfg(
context.properties = {
    support.dbus      = false
    mem.allow-mlock   = false
    core.daemon       = true
    core.name         = pipewire-0
}

context.spa-libs = {
    support.*       = support/libspa-support
    audio.convert.* = audioconvert/libspa-audioconvert
}

context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-spa-device-factory }
]

context.objects = [
    { factory = spa-node-factory
        args = {
            factory.name     = support.null-audio-sink
            node.name        = "test-null-sink"
            media.class      = Audio/Sink
            object.linger    = true
            audio.position   = [ FL FR ]
            priority.driver  = 30000
        }
    }
]
)pwcfg"_ba;

        file.write(config);
        file.close();
        return configPath;
    }

    std::unique_ptr<QProcess> m_process;
    QString m_socketPath;
    QTemporaryDir m_configDir;
};
