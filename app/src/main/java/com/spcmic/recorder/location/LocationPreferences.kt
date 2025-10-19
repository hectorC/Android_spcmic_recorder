package com.spcmic.recorder.location

import android.content.Context

/**
 * Small helper for persisting the geolocation capture toggle.
 */
object LocationPreferences {
    private const val PREFS_NAME = "geolocation_prefs"
    private const val KEY_CAPTURE_ENABLED = "capture_enabled"

    fun isCaptureEnabled(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        return prefs.getBoolean(KEY_CAPTURE_ENABLED, false)
    }

    fun setCaptureEnabled(context: Context, enabled: Boolean) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit().putBoolean(KEY_CAPTURE_ENABLED, enabled).apply()
    }
}
