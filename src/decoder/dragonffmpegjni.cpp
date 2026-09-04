/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

#include "dragonffmpegjni.h"

#include <QtGlobal>

#ifdef Q_OS_ANDROID

#include "dragonmediabackend_decode_logging.h"

#include <QtCore/qcoreapplication_platform.h>
#include <QtCore/qjnienvironment.h>
#include <QtCore/qjniobject.h>

extern "C" {
#include <libavcodec/jni.h>
}

#include <mutex>

#endif

namespace DragonMediaBackend
{

void initAndroidJniBridge()
{
#ifdef Q_OS_ANDROID
    static std::once_flag once;
    std::call_once(once, [] {
        QJniEnvironment env;
        if (const int ret = av_jni_set_java_vm(env.javaVM(), nullptr); ret < 0) {
            qCWarning(dragonMediaBackendDecode) << "av_jni_set_java_vm failed:" << ret;
            return;
        }

        const QJniObject context = QNativeInterface::QAndroidApplication::context();
        if (!context.isValid()) {
            qCWarning(dragonMediaBackendDecode) << "QAndroidApplication::context() unavailable, content:// playback disabled";
            return;
        }

        const jobject appCtx = env->NewGlobalRef(context.object());
        if (!appCtx) {
            qCWarning(dragonMediaBackendDecode) << "NewGlobalRef(app context) failed";
            return;
        }
        if (const int ret = av_jni_set_android_app_ctx(appCtx, nullptr); ret < 0) {
            qCWarning(dragonMediaBackendDecode) << "av_jni_set_android_app_ctx failed:" << ret;
            return;
        }

        qCDebug(dragonMediaBackendDecode) << "FFmpeg JNI bridge initialized (content:// protocol available)";
    });
#endif
}

}
