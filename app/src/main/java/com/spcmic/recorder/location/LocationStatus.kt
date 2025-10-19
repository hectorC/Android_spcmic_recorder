package com.spcmic.recorder.location

/**
 * UI-facing status of the location capture system.
 */
sealed class LocationStatus {
    /** Location capture is disabled by user preference. */
    object Disabled : LocationStatus()

    /** Location capture is enabled but no session is currently active. */
    object Idle : LocationStatus()

    /** A session is running and we are waiting for a fix. */
    object Acquiring : LocationStatus()

    /** A fix is available for the current session. */
    data class Ready(val accuracyMeters: Float?) : LocationStatus()

    /** Location is unavailable (e.g., provider off, settings blocked). */
    data class Unavailable(val reason: String? = null) : LocationStatus()

    /** User denied the required location permission. */
    object PermissionDenied : LocationStatus()
}
