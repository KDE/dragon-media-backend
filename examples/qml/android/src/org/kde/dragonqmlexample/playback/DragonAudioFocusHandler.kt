/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

package org.kde.dragonqmlexample.playback

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

class DragonAudioFocusHandler(private val context: Context) {

    private val audioManager =
        context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val mainHandler = Handler(Looper.getMainLooper())

    private var focusRequest: AudioFocusRequest? = null
    private var pausedByFocusLoss = false
    private var noisyReceiverRegistered = false

    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            -> {
                if (DragonBridge.isPlaying) {
                    pausedByFocusLoss = true
                    DragonBridge.nativePause()
                }
            }
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> {
                pausedByFocusLoss = false
                DragonBridge.nativeSetDucking(true)
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                DragonBridge.nativeSetDucking(false)
                if (pausedByFocusLoss) {
                    pausedByFocusLoss = false
                    DragonBridge.nativePlay()
                }
            }
        }
    }

    private val noisyReceiver =
        object : BroadcastReceiver() {
            override fun onReceive(receiverContext: Context, intent: Intent) {
                if (intent.action == AudioManager.ACTION_AUDIO_BECOMING_NOISY &&
                        DragonBridge.isPlaying) {
                    DragonBridge.nativePause()
                }
            }
        }

    fun onPlayWhenReadyChanged(playing: Boolean) {
        if (playing) {
            requestFocus()
            registerNoisyReceiver()
        } else {
            abandonFocus()
            unregisterNoisyReceiver()
        }
    }

    fun release() {
        pausedByFocusLoss = false
        DragonBridge.nativeSetDucking(false)
        abandonFocus()
        unregisterNoisyReceiver()
    }

    private fun requestFocus() {
        if (focusRequest == null) {
            val attributes =
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build()
            focusRequest =
                AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                    .setAudioAttributes(attributes)
                    .setOnAudioFocusChangeListener(focusListener, mainHandler)
                    .build()
        }
        audioManager.requestAudioFocus(focusRequest!!)
    }

    private fun abandonFocus() {
        focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
    }

    private fun registerNoisyReceiver() {
        if (!noisyReceiverRegistered) {
            context.registerReceiver(
                noisyReceiver,
                IntentFilter(AudioManager.ACTION_AUDIO_BECOMING_NOISY),
            )
            noisyReceiverRegistered = true
        }
    }

    private fun unregisterNoisyReceiver() {
        if (noisyReceiverRegistered) {
            context.unregisterReceiver(noisyReceiver)
            noisyReceiverRegistered = false
        }
    }
}
