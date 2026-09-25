/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "stream/logging_timestamp.h"
#include "visualizationcontroller.h"
#include <DragonMediaBackend/dragonaudiooutput.h>
#include <DragonMediaBackend/dragonplayer.h>
#include <DragonMediaBackend/dragonspectrumanalyzer.h>

#include <QFileInfo>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QStringView>
#include <QTimer>
#include <QUrl>

#include <QtQml/qqml.h>
#include <QtQml/qqmlapplicationengine.h>
#include <QtQml/qqmlcontext.h>
#include <chrono>

#ifdef Q_OS_ANDROID
#include "mediasessioncontroller.h"
#include <SDL3/SDL_hints.h>
#endif

static void enableDebugOutput()
{
    QLoggingCategory::setFilterRules(
        QStringLiteral("org.kde.dragonmediabackend.*.debug=true\n"
                       "qt.qpa.*=false\n"
                       "qt.network.*=false\n"
                       "qt.dbus*=false\n"
                       "qt.svg*=false"));
}

struct EnableDebugOutput {
    EnableDebugOutput()
    {
        DragonMediaBackend_install_timestamped_handler();
        enableDebugOutput();
    }
};

static const EnableDebugOutput enableDebugOutputInstance;

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("dragonqml_example"));

    DragonPlayer player;
#ifdef Q_OS_ANDROID
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    DragonAndroidMediaSessionController mediaSessionController(&player);
#endif
    auto *analyzer = new DragonSpectrumAnalyzer(&player, &player);
    analyzer->setMode(DragonSpectrumAnalyzer::Mode::DetailedOnly);
    auto *visualization = new VisualizationController(analyzer, &player);

    QUrl defaultUrl;
    const QString sample = QStringLiteral("research/samples/sample-3s.mp3");
    if (QFile::exists(sample)) {
        defaultUrl = QUrl::fromLocalFile(QFileInfo(sample).absoluteFilePath());
    }

    qmlRegisterType<DragonPlayer>("org.kde.dragonqmlexample.types", 1, 0, "DragonPlayer");

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("player"), &player);
    engine.rootContext()->setContextProperty(QStringLiteral("audioOutput"), player.audioOutput());
    engine.rootContext()->setContextProperty(QStringLiteral("spectrum"), visualization);
    engine.rootContext()->setContextProperty(QStringLiteral("defaultUrl"), defaultUrl);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [] {
            QCoreApplication::exit(1);
        },
        Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("org.kde.dragonqmlexample"), QStringLiteral("Main"));

    QString autoplay = qEnvironmentVariable("DRAGON_QML_EXAMPLE_AUTOPLAY");
    const QStringList args = QCoreApplication::arguments();
    for (int i = 0; i < args.size() && autoplay.isEmpty(); ++i) {
        if (args.at(i) == QStringLiteral("--autoplay") && i + 1 < args.size()) {
            autoplay = args.at(i + 1);
        }
    }
    qint64 autoplaySeekMs = -1;
    if (autoplay.isEmpty()) {
        const QString triggerPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/autoplay.txt");
        qDebug() << "autoplay trigger path:" << triggerPath;
        QFile triggerFile(triggerPath);
        if (triggerFile.open(QIODevice::ReadOnly)) {
            const QStringList lines = QString::fromUtf8(triggerFile.readAll()).split(u'\n');
            triggerFile.remove();
            autoplay = lines.value(0).trimmed();
            for (const QString &line : lines) {
                const QString seek = QStringLiteral("seek:");
                if (line.startsWith(seek, Qt::CaseInsensitive)) {
                    bool ok = false;
                    const qint64 ms = QStringView{line}.mid(seek.size()).trimmed().toLongLong(&ok);
                    if (ok && ms >= 0) {
                        autoplaySeekMs = ms;
                    }
                }
            }
        }
    }
    qDebug() << "autoplay candidate:" << autoplay << "args:" << args;
    if (!autoplay.isEmpty()) {
        const QUrl source = QFile::exists(autoplay) ? QUrl::fromLocalFile(QFileInfo(autoplay).absoluteFilePath()) : QUrl(autoplay);
        qDebug() << "autoplay: setSource" << source;
        player.setSource(source);
        player.play();
        if (autoplaySeekMs >= 0) {
            QTimer::singleShot(1500, &player, [seekMs = autoplaySeekMs, &player]() {
                qDebug() << "autoplay: seek to" << seekMs << "ms (seekable:" << player.seekable() << ")";
                player.setPosition(std::chrono::milliseconds{seekMs});
            });
        }
    }

    return app.exec();
}
