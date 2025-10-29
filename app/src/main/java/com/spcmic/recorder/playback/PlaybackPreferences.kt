package com.spcmic.recorder.playback

import android.content.Context
import android.content.SharedPreferences

object PlaybackPreferences {
    private const val PREF_KEY_REALTIME_CONVOLVED = "pref_realtime_convolved_playback"

    private fun prefs(context: Context): SharedPreferences {
        return context.getSharedPreferences(PlaybackViewModel.PREFS_NAME, Context.MODE_PRIVATE)
    }

    fun isRealtimeConvolutionEnabled(context: Context): Boolean {
        return prefs(context).getBoolean(PREF_KEY_REALTIME_CONVOLVED, true)
    }

    fun setRealtimeConvolutionEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean(PREF_KEY_REALTIME_CONVOLVED, enabled).apply()
    }

    fun keyRealtimeConvolution(): String = PREF_KEY_REALTIME_CONVOLVED
}
