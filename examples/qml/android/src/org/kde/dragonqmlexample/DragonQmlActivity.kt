/*
 * SPDX-FileCopyrightText: 2026 Ian Monroe <imonroe@kde.org>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
 */

package org.kde.dragonqmlexample

import android.os.Build
import android.os.Bundle
import android.util.Log
import org.libsdl.app.SDL
import org.libsdl.app.SDLActivity
import org.qtproject.qt.android.bindings.QtActivity

class DragonQmlActivity : QtActivity() {

    companion object {
        private const val TAG = "DragonQmlActivity"

        private fun initSdlForActivity(activity: DragonQmlActivity) {
            try {
                if (Build.SUPPORTED_ABIS != null && Build.SUPPORTED_ABIS.isNotEmpty()) {
                    try {
                        System.loadLibrary("SDL3_" + Build.SUPPORTED_ABIS[0])
                    } catch (ignored: UnsatisfiedLinkError) {
                        System.loadLibrary("SDL3")
                    }
                } else {
                    System.loadLibrary("SDL3")
                }
                SDL.setupJNI()
                SDL.initialize()
                SDL.setContext(activity)
            } catch (e: UnsatisfiedLinkError) {
                Log.w(TAG, "SDL3 native library not present; skipping SDL JNI setup: " + e.message)
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        initSdlForActivity(this)
        super.onCreate(savedInstanceState)
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            SDLActivity.nativeQuit()
            SDL.setContext(null)
        } catch (ignored: UnsatisfiedLinkError) {
        }
    }
}
