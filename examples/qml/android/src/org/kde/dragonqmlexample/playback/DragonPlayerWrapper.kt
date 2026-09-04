/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

package org.kde.dragonqmlexample.playback

import android.os.Looper
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.util.UnstableApi
import androidx.media3.common.util.Util
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture

@UnstableApi
class DragonPlayerWrapper : SimpleBasePlayer(Looper.getMainLooper()) {

    override fun getState(): State = buildState()

    override fun handleSetPlayWhenReady(playWhenReady: Boolean): ListenableFuture<*> {
        if (playWhenReady) {
            DragonBridge.nativePlay()
        } else {
            DragonBridge.nativePause()
        }
        return Futures.immediateVoidFuture()
    }

    override fun handleSeek(
        mediaItemIndex: Int,
        positionMs: Long,
        @Player.Command seekCommand: Int,
    ): ListenableFuture<*> {
        if (DragonBridge.seekable) {
            DragonBridge.nativeSeekTo(positionMs)
        }
        return Futures.immediateVoidFuture()
    }

    override fun handleStop(): ListenableFuture<*> {
        DragonBridge.nativeStop()
        return Futures.immediateVoidFuture()
    }

    override fun handleRelease(): ListenableFuture<*> = Futures.immediateVoidFuture()

    fun invalidateStateSafe() {
        verifyApplicationThread()
        invalidateState()
    }

    private fun buildState(): State {
        val bridge = DragonBridge
        val builder = State.Builder()

        val commands =
            Player.Commands.Builder()
                .add(Player.COMMAND_PLAY_PAUSE)
                .add(Player.COMMAND_STOP)
                .add(Player.COMMAND_GET_CURRENT_MEDIA_ITEM)
                .add(Player.COMMAND_GET_TIMELINE)
                .add(Player.COMMAND_GET_METADATA)
                .apply {
                    if (bridge.seekable) {
                        add(Player.COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM)
                        add(Player.COMMAND_SEEK_BACK)
                        add(Player.COMMAND_SEEK_FORWARD)
                    }
                }
                .build()
        builder.setAvailableCommands(commands)

        if (bridge.source.isEmpty()) {
            return builder.setPlaybackState(Player.STATE_IDLE).build()
        }

        val playbackState =
            when {
                bridge.playbackState == PLAYBACK_STATE_STOPPED &&
                    bridge.mediaStatus == MEDIA_STATUS_END_OF_MEDIA ->
                    Player.STATE_ENDED
                bridge.playbackState == PLAYBACK_STATE_STOPPED -> Player.STATE_IDLE
                bridge.mediaStatus == MEDIA_STATUS_LOADING ||
                    bridge.mediaStatus == MEDIA_STATUS_BUFFERING -> Player.STATE_BUFFERING
                else -> Player.STATE_READY
            }
        builder.setPlaybackState(playbackState)
        builder.setSeekBackIncrementMs(SEEK_INCREMENT_MS)
        builder.setSeekForwardIncrementMs(SEEK_INCREMENT_MS)

        if (bridge.errorCode != 0 && bridge.playbackState == PLAYBACK_STATE_STOPPED) {
            builder.setPlaybackState(Player.STATE_IDLE)
            builder.setPlayerError(
                PlaybackException(
                    bridge.errorString,
                    null,
                    PlaybackException.ERROR_CODE_UNSPECIFIED,
                )
            )
        }

        val playWhenReady = bridge.playbackState == PLAYBACK_STATE_PLAYING
        builder.setPlayWhenReady(
            playWhenReady,
            Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST,
        )

        val metadata =
            MediaMetadata.Builder()
                .setTitle(bridge.title.ifEmpty { bridge.source })
                .build()
        val mediaItem =
            MediaItem.Builder()
                .setMediaId(bridge.source)
                .setUri(bridge.source)
                .setMediaMetadata(metadata)
                .build()
        val durationUs = if (bridge.durationMs > 0) Util.msToUs(bridge.durationMs) else C.TIME_UNSET
        val mediaItemData =
            MediaItemData.Builder(bridge.source)
                .setMediaItem(mediaItem)
                .setDurationUs(durationUs)
                .setIsSeekable(bridge.seekable)
                .build()

        builder
            .setPlaylist(listOf(mediaItemData))
            .setCurrentMediaItemIndex(0)
            .setContentPositionMs(bridge.positionMs)

        return builder.build()
    }

    private companion object {
        private const val PLAYBACK_STATE_STOPPED = 0
        private const val PLAYBACK_STATE_PLAYING = 1
        private const val PLAYBACK_STATE_PAUSED = 2
        private const val MEDIA_STATUS_LOADING = 1
        private const val MEDIA_STATUS_BUFFERING = 3
        private const val MEDIA_STATUS_END_OF_MEDIA = 6
        private const val SEEK_INCREMENT_MS = 10_000L
    }
}
