/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "mediasessioncontroller.h"

#include <DragonMediaBackend/dragonaudiooutput.h>
#include <DragonMediaBackend/dragonicymetadata.h>
#include <DragonMediaBackend/dragonplayer.h>

#include <QtCore/qcoreapplication_platform.h>
#include <QtCore/qjnienvironment.h>
#include <QtCore/qjniobject.h>

#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include <chrono>

namespace
{
constexpr char kBridgeClass[] = "org/kde/dragonqmlexample/playback/DragonBridge";
constexpr char kServiceClass[] = "org.kde.dragonqmlexample.playback.DragonMediaSessionService";

DragonPlayer *g_player = nullptr;
qreal g_preDuckVolume = -1.0;

void nativePlay(JNIEnv *, jobject)
{
    QMetaObject::invokeMethod(g_player, "play", Qt::QueuedConnection);
}

void nativePause(JNIEnv *, jobject)
{
    QMetaObject::invokeMethod(g_player, "pause", Qt::QueuedConnection);
}

void nativeStop(JNIEnv *, jobject)
{
    QMetaObject::invokeMethod(g_player, "stop", Qt::QueuedConnection);
}

void nativeSeekTo(JNIEnv *, jobject, jlong positionMs)
{
    QMetaObject::invokeMethod(g_player, "setPosition", Qt::QueuedConnection, Q_ARG(std::chrono::milliseconds, std::chrono::milliseconds{positionMs}));
}

void nativeSetDucking(JNIEnv *, jobject, jboolean duck)
{
    const qreal scale = duck ? 0.2 : 1.0;
    QMetaObject::invokeMethod(
        g_player,
        [duck, scale]() {
            auto *output = g_player->audioOutput();
            if (output == nullptr) {
                return;
            }
            if (duck) {
                g_preDuckVolume = output->volume();
                output->setVolume(g_preDuckVolume * scale);
            } else if (g_preDuckVolume >= 0.0) {
                output->setVolume(g_preDuckVolume);
                g_preDuckVolume = -1.0;
            }
        },
        Qt::QueuedConnection);
}

void pushLongMethod(const char *method, const char *signature, jlong value)
{
    QJniObject::callStaticMethod<void>(kBridgeClass, method, signature, value);
}

void pushIntMethod(const char *method, const char *signature, jint value)
{
    QJniObject::callStaticMethod<void>(kBridgeClass, method, signature, value);
}

void pushBoolMethod(const char *method, const char *signature, jboolean value)
{
    QJniObject::callStaticMethod<void>(kBridgeClass, method, signature, value);
}

void pushStringMethod(const char *method, const char *signature, const QString &value)
{
    QJniObject::callStaticMethod<void>(kBridgeClass, method, signature, QJniObject::fromString(value).object<jstring>());
}

void pushError(DragonPlayer::Error error, const QString &message)
{
    QJniObject::callStaticMethod<void>(kBridgeClass,
                                       "onErrorChanged",
                                       "(ILjava/lang/String;)V",
                                       jint(error),
                                       QJniObject::fromString(message).object<jstring>());
}
}

DragonAndroidMediaSessionController::DragonAndroidMediaSessionController(DragonPlayer *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
    Q_ASSERT(g_player == nullptr);
    g_player = player;

    {
        QJniEnvironment env;
        const JNINativeMethod methods[] = {
            {"nativePlay", "()V", reinterpret_cast<void *>(nativePlay)},
            {"nativePause", "()V", reinterpret_cast<void *>(nativePause)},
            {"nativeStop", "()V", reinterpret_cast<void *>(nativeStop)},
            {"nativeSeekTo", "(J)V", reinterpret_cast<void *>(nativeSeekTo)},
            {"nativeSetDucking", "(Z)V", reinterpret_cast<void *>(nativeSetDucking)},
        };
        const jclass clazz = env.findClass(kBridgeClass);
        env.registerNativeMethods(clazz, methods, sizeof(methods) / sizeof(methods[0]));
    }

    pushIntMethod("onStateChanged", "(I)V", jint(player->playbackState()));
    pushIntMethod("onStatusChanged", "(I)V", jint(player->status()));
    pushLongMethod("onDurationChanged", "(J)V", jlong(player->duration().value_or(std::chrono::milliseconds{0}).count()));
    pushBoolMethod("onSeekableChanged", "(Z)V", jboolean(player->seekable()));
    pushError(player->error(), player->errorString());

    connect(player, &DragonPlayer::stateChanged, this, [this](DragonPlayer::PlaybackState newState, DragonPlayer::PlaybackState) {
        pushIntMethod("onStateChanged", "(I)V", jint(newState));
        if (newState == DragonPlayer::PlaybackState::PlayingState) {
            m_serviceStopPending = false;
            startMediaSessionService();
            m_positionTimer->start();
        } else if (newState == DragonPlayer::PlaybackState::StoppedState && m_serviceStarted) {
            m_serviceStopPending = true;
            QTimer::singleShot(500, this, [this]() {
                if (m_serviceStopPending && m_player->playbackState() == DragonPlayer::PlaybackState::StoppedState) {
                    stopMediaSessionService();
                }
            });
        }
    });

    connect(player, &DragonPlayer::statusChanged, this, [](DragonPlayer::MediaStatus status) {
        pushIntMethod("onStatusChanged", "(I)V", jint(status));
    });

    connect(player, &DragonPlayer::durationChanged, this, [](std::optional<std::chrono::milliseconds> duration) {
        pushLongMethod("onDurationChanged", "(J)V", jlong(duration ? duration->count() : 0));
    });

    connect(player, &DragonPlayer::seekableChanged, this, [](bool seekable) {
        pushBoolMethod("onSeekableChanged", "(Z)V", jboolean(seekable));
    });

    connect(player, &DragonPlayer::sourceChanged, this, [this]() {
        pushSourceTitle();
    });

    connect(player, &DragonPlayer::currentPlayingForRadiosChanged, this, [](const DragonIcyMetadata &metadata) {
        pushStringMethod("onTitleChanged", "(Ljava/lang/String;)V", metadata.streamTitle());
    });

    connect(player, &DragonPlayer::errorChanged, this, [player](DragonPlayer::Error error) {
        pushError(error, player->errorString());
    });

    m_positionTimer = new QTimer(this);
    m_positionTimer->setInterval(500);
    connect(m_positionTimer, &QTimer::timeout, this, [this]() {
        if (m_player->playbackState() == DragonPlayer::PlaybackState::PlayingState) {
            pushLongMethod("onPositionChanged", "(J)V", jlong(m_player->position().count()));
        } else {
            m_positionTimer->stop();
        }
    });
}

DragonAndroidMediaSessionController::~DragonAndroidMediaSessionController()
{
    if (m_serviceStarted) {
        stopMediaSessionService();
    }
    g_player = nullptr;
}

void DragonAndroidMediaSessionController::pushSourceTitle()
{
    const QUrl source = m_player->source();
    pushStringMethod("onSourceChanged", "(Ljava/lang/String;)V", source.toString());
    if (source.isLocalFile()) {
        pushStringMethod("onTitleChanged", "(Ljava/lang/String;)V", QFileInfo(source.toLocalFile()).fileName());
        QMetaObject::invokeMethod(
            m_player,
            [name = QFileInfo(source.toLocalFile()).fileName()]() {
                g_player->setStreamName(name);
            },
            Qt::QueuedConnection);
    }
}

void DragonAndroidMediaSessionController::startMediaSessionService()
{
    if (m_serviceStarted) {
        return;
    }
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    QJniObject intent("android/content/Intent");
    intent.callObjectMethod("setClassName",
                            "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",
                            context.object<jobject>(),
                            QJniObject::fromString(QString::fromLatin1(kServiceClass)).object<jstring>());
    context.callMethod<jobject>("startForegroundService", "(Landroid/content/Intent;)Landroid/content/ComponentName;", intent.object<jobject>());
    m_serviceStarted = true;
}

void DragonAndroidMediaSessionController::stopMediaSessionService()
{
    if (!m_serviceStarted) {
        return;
    }
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    QJniObject intent("android/content/Intent");
    intent.callObjectMethod("setClassName",
                            "(Landroid/content/Context;Ljava/lang/String;)Landroid/content/Intent;",
                            context.object<jobject>(),
                            QJniObject::fromString(QString::fromLatin1(kServiceClass)).object<jstring>());
    context.callMethod<jboolean>("stopService", "(Landroid/content/Intent;)Z", intent.object<jobject>());
    m_serviceStarted = false;
    m_serviceStopPending = false;
}
