package com.spcmic.recorder.location

/**
 * Represents a single geolocation fix captured for a recording session.
 */
data class RecordingLocation(
    val latitude: Double,
    val longitude: Double,
    val accuracyMeters: Float?,
    val altitudeMeters: Double?,
    val timestampMillisUtc: Long,
    val provider: String?
)
