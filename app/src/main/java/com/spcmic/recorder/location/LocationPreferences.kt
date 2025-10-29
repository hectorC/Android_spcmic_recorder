package com.spcmic.recorder.location

import android.content.Context

/**
 * Small helper for persisting the geolocation capture toggle.
 */
object LocationPreferences {
    private const val PREFS_NAME = "geolocation_prefs"
    private const val KEY_CAPTURE_ENABLED = "capture_enabled"
    private const val KEY_USER_CONFIRMED = "capture_user_confirmed"

    fun isCaptureEnabled(context: Context): Boolean {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        if (!prefs.contains(KEY_USER_CONFIRMED)) {
            if (prefs.contains(KEY_CAPTURE_ENABLED) && prefs.getBoolean(KEY_CAPTURE_ENABLED, false)) {
                // Legacy versions auto-enabled capture; reset to disabled until the user opts in.
                prefs.edit().putBoolean(KEY_CAPTURE_ENABLED, false).apply()
            }
            return false
        }
        return prefs.getBoolean(KEY_CAPTURE_ENABLED, false)
    }

    fun setCaptureEnabled(context: Context, enabled: Boolean) {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putBoolean(KEY_CAPTURE_ENABLED, enabled)
            .putBoolean(KEY_USER_CONFIRMED, true)
            .apply()
    }
}
