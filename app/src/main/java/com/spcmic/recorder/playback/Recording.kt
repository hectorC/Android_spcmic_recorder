package com.spcmic.recorder.playback

import java.io.File

/**
 * Data class representing a recorded audio file with its metadata
 */
data class Recording(
    val file: File,
    val fileName: String,
    val dateTime: Long,
    val durationMs: Long,
    val sampleRate: Int,
    val channels: Int,
    val fileSizeBytes: Long
) {
    val fileSizeMB: Float
        get() = fileSizeBytes / (1024f * 1024f)
    
    val durationSeconds: Int
        get() = (durationMs / 1000).toInt()
    
    val formattedDuration: String
        get() {
            val totalSeconds = durationSeconds
            val minutes = totalSeconds / 60
            val seconds = totalSeconds % 60
            return String.format("%d:%02d", minutes, seconds)
        }
    
    val formattedSampleRate: String
        get() = "${sampleRate / 1000} kHz"
    
    val formattedFileSize: String
        get() = when {
            fileSizeMB >= 1024 -> String.format("%.1f GB", fileSizeMB / 1024f)
            fileSizeMB >= 1 -> String.format("%.1f MB", fileSizeMB)
            else -> String.format("%.0f KB", fileSizeBytes / 1024f)
        }
}
