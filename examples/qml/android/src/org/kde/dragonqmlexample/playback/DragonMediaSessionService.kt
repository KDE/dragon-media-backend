/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

package org.kde.dragonqmlexample.playback

import android.app.PendingIntent
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.DefaultMediaNotificationProvider
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import org.kde.dragonqmlexample.R

@UnstableApi
class DragonMediaSessionService : MediaSessionService() {

    private var mediaSession: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        val wrapper = DragonPlayerWrapper()
        mediaSession =
            MediaSession.Builder(this, wrapper)
                .setSessionActivity(launcherActivityPendingIntent())
                .build()
        setMediaNotificationProvider(notificationProvider())
        addSession(mediaSession!!)
        DragonBridge.attachSession(this, wrapper)
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? =
        mediaSession

    override fun onDestroy() {
        DragonBridge.detachSession()
        mediaSession?.release()
        mediaSession = null
        super.onDestroy()
    }

    private fun launcherActivityPendingIntent(): PendingIntent {
        val launchIntent = packageManager.getLaunchIntentForPackage(packageName)
        return PendingIntent.getActivity(
            this,
            0,
            launchIntent,
            PendingIntent.FLAG_IMMUTABLE,
        )
    }

    private fun notificationProvider(): DefaultMediaNotificationProvider =
        DefaultMediaNotificationProvider.Builder(this)
            .setChannelId(NOTIFICATION_CHANNEL_ID)
            .setChannelName(R.string.dragon_notification_channel)
            .build()
            .apply { setSmallIcon(R.drawable.ic_dragon_notification) }

    private companion object {
        private const val NOTIFICATION_CHANNEL_ID = "dragon_playback"
    }
}
