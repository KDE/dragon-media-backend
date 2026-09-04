/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

package org.kde.dragonqmlexample.playback

import android.content.Context
import android.os.Handler
import android.os.Looper

object DragonBridge {
    private val mainHandler = Handler(Looper.getMainLooper())

    @Volatile var playbackState: Int = 0
    @Volatile var mediaStatus: Int = 0
    @Volatile var durationMs: Long = 0
    @Volatile var positionMs: Long = 0
    @Volatile var seekable: Boolean = false
    @Volatile var source: String = ""
    @Volatile var title: String = ""
    @Volatile var errorCode: Int = 0
    @Volatile var errorString: String = ""

    @Volatile var wrapper: DragonPlayerWrapper? = null
    @Volatile var focusHandler: DragonAudioFocusHandler? = null

    val isPlaying: Boolean
        get() = playbackState == 1

    @JvmStatic
    fun onStateChanged(state: Int) {
        playbackState = state
        invalidate()
        postToMain { focusHandler?.onPlayWhenReadyChanged(isPlaying) }
    }

    @JvmStatic
    fun onStatusChanged(status: Int) {
        mediaStatus = status
        invalidate()
    }

    @JvmStatic
    fun onDurationChanged(duration: Long) {
        durationMs = duration
        invalidate()
    }

    @JvmStatic
    fun onSeekableChanged(canSeek: Boolean) {
        seekable = canSeek
        invalidate()
    }

    @JvmStatic
    fun onPositionChanged(position: Long) {
        positionMs = position
        invalidate()
    }

    @JvmStatic
    fun onSourceChanged(newSource: String) {
        source = newSource
        invalidate()
    }

    @JvmStatic
    fun onTitleChanged(newTitle: String) {
        title = newTitle
        invalidate()
    }

    @JvmStatic
    fun onErrorChanged(code: Int, message: String) {
        errorCode = code
        errorString = message
        invalidate()
    }

    external fun nativePlay()
    external fun nativePause()
    external fun nativeStop()
    external fun nativeSeekTo(positionMs: Long)

    external fun nativeSetDucking(duck: Boolean)

    fun postToMain(action: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            action()
        } else {
            mainHandler.post(action)
        }
    }

    private fun invalidate() {
        val currentWrapper = wrapper ?: return
        postToMain { currentWrapper.invalidateStateSafe() }
    }

    fun attachSession(context: Context, sessionWrapper: DragonPlayerWrapper) {
        wrapper = sessionWrapper
        if (focusHandler == null) {
            focusHandler = DragonAudioFocusHandler(context)
        }
        focusHandler?.onPlayWhenReadyChanged(isPlaying)
    }

    fun detachSession() {
        focusHandler?.release()
        wrapper = null
    }
}
