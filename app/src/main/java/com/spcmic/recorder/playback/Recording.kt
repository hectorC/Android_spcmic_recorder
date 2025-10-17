package com.spcmic.recorder.playback

import android.content.Context
import android.net.Uri
import androidx.documentfile.provider.DocumentFile
import java.io.File

/**
 * Data class representing a recorded audio file with its metadata
 */
data class Recording(
    val file: File?,
    val documentUri: Uri?,
    val displayPath: String,
    val fileName: String,
    val dateTime: Long,
    val durationMs: Long,
    val sampleRate: Int,
    val channels: Int,
    val fileSizeBytes: Long
) {
    val fileSizeMB: Float
        get() = fileSizeBytes / (1024f * 1024f)

    val absolutePath: String?
        get() = file?.absolutePath ?: displayPath

    val uniqueId: String
        get() = absolutePath ?: documentUri?.toString() ?: fileName

    val cacheKey: String?
        get() = file?.absolutePath ?: documentUri?.toString() ?: displayPath.takeIf { it.isNotBlank() }
    
    val durationSeconds: Long
        get() = durationMs / 1000L
    
    val formattedDuration: String
        get() {
            val totalSeconds = durationSeconds
            val hours = totalSeconds / 3600
            val minutes = (totalSeconds % 3600) / 60
            val seconds = totalSeconds % 60
            return if (hours > 0) {
                String.format("%d:%02d:%02d", hours, minutes, seconds)
            } else {
                String.format("%d:%02d", minutes, seconds)
            }
        }
    
    val formattedSampleRate: String
        get() = "${sampleRate / 1000} kHz"
    
    val formattedFileSize: String
        get() = when {
            fileSizeMB >= 1024 -> String.format("%.1f GB", fileSizeMB / 1024f)
            fileSizeMB >= 1 -> String.format("%.1f MB", fileSizeMB)
            else -> String.format("%.0f KB", fileSizeBytes / 1024f)
        }

    fun delete(context: Context): Boolean {
        file?.let {
            if (it.exists() && it.delete()) {
                return true
            }
        }

        documentUri?.let {
            val document = DocumentFile.fromSingleUri(context, it)
            if (document != null && document.isFile) {
                return document.delete()
            }
        }
        return false
    }
}
