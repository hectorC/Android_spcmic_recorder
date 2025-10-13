package com.spcmic.recorder.playback

import android.content.Context
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.BufferedInputStream
import java.io.File
import java.io.FileInputStream
import java.io.InputStream
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Utility class for parsing WAV file metadata without loading the entire file
 */
object WavMetadataParser {
    private const val TAG = "WavMetadataParser"
    
    data class WavMetadata(
        val sampleRate: Int,
        val channels: Int,
        val bitsPerSample: Int,
        val dataSize: Long,
        val durationMs: Long
    )
    
    /**
     * Parse WAV file header to extract metadata
     */
    fun parseMetadata(file: File): WavMetadata? {
        if (!file.exists() || !file.name.endsWith(".wav", ignoreCase = true)) {
            return null
        }
        
        try {
            RandomAccessFile(file, "r").use { raf ->
                // Read RIFF header
                val riffHeader = ByteArray(12)
                raf.read(riffHeader)
                
                val riff = String(riffHeader, 0, 4)
                if (riff != "RIFF") {
                    Log.e(TAG, "Not a valid WAV file: $file")
                    return null
                }
                
                val wave = String(riffHeader, 8, 4)
                if (wave != "WAVE") {
                    Log.e(TAG, "Not a valid WAVE file: $file")
                    return null
                }
                
                // Find fmt chunk
                var sampleRate = 0
                var channels = 0
                var bitsPerSample = 0
                var dataSize = 0L
                
                while (raf.filePointer < raf.length()) {
                    val chunkHeader = ByteArray(8)
                    val bytesRead = raf.read(chunkHeader)
                    if (bytesRead < 8) break
                    
                    val chunkId = String(chunkHeader, 0, 4)
                    val chunkSize = ByteBuffer.wrap(chunkHeader, 4, 4)
                        .order(ByteOrder.LITTLE_ENDIAN)
                        .int
                        .toLong() and 0xFFFFFFFFL
                    
                    when (chunkId) {
                        "fmt " -> {
                            // Read format chunk
                            val fmtData = ByteArray(chunkSize.toInt())
                            raf.read(fmtData)
                            
                            val buffer = ByteBuffer.wrap(fmtData).order(ByteOrder.LITTLE_ENDIAN)
                            buffer.short // audio format
                            channels = buffer.short.toInt()
                            sampleRate = buffer.int
                            buffer.int // byte rate
                            buffer.short // block align
                            bitsPerSample = buffer.short.toInt()
                        }
                        "data" -> {
                            dataSize = chunkSize
                            break // We have all we need
                        }
                        else -> {
                            // Skip unknown chunk
                            raf.seek(raf.filePointer + chunkSize)
                        }
                    }
                }
                
                if (sampleRate == 0 || channels == 0 || bitsPerSample == 0 || dataSize == 0L) {
                    Log.e(TAG, "Invalid WAV metadata for file: $file")
                    return null
                }
                
                // Calculate duration
                val bytesPerSample = bitsPerSample / 8
                val totalSamples = dataSize / (channels * bytesPerSample)
                val durationMs = (totalSamples * 1000) / sampleRate
                
                return WavMetadata(
                    sampleRate = sampleRate,
                    channels = channels,
                    bitsPerSample = bitsPerSample,
                    dataSize = dataSize,
                    durationMs = durationMs
                )
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing WAV file: $file", e)
            return null
        }
    }

    fun parseMetadata(context: Context, documentFile: DocumentFile): WavMetadata? {
        if (!documentFile.isFile) {
            return null
        }

        val name = documentFile.name ?: return null
        if (!name.endsWith(".wav", ignoreCase = true)) {
            return null
        }

        return try {
            context.contentResolver.openFileDescriptor(documentFile.uri, "r")?.use { pfd ->
                FileInputStream(pfd.fileDescriptor).use { fis ->
                    parseMetadataFromStream(BufferedInputStream(fis))
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error parsing WAV document: ${documentFile.uri}", e)
            null
        }
    }

    private fun parseMetadataFromStream(stream: InputStream): WavMetadata? {
        val header = ByteArray(12)
        if (!stream.readFully(header)) {
            return null
        }

        val riff = String(header, 0, 4)
        if (riff != "RIFF") {
            return null
        }

        val wave = String(header, 8, 4)
        if (wave != "WAVE") {
            return null
        }

        var sampleRate = 0
        var channels = 0
        var bitsPerSample = 0
        var dataSize = 0L

        val chunkHeader = ByteArray(8)

        while (stream.readFully(chunkHeader)) {
            val chunkId = String(chunkHeader, 0, 4)
            val chunkSize = ByteBuffer.wrap(chunkHeader, 4, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .int
                .toLong() and 0xFFFFFFFFL

            when (chunkId) {
                "fmt " -> {
                    val fmtData = ByteArray(chunkSize.toInt())
                    if (!stream.readFully(fmtData)) {
                        return null
                    }

                    val buffer = ByteBuffer.wrap(fmtData).order(ByteOrder.LITTLE_ENDIAN)
                    buffer.short // audio format
                    channels = buffer.short.toInt()
                    sampleRate = buffer.int
                    buffer.int // byte rate
                    buffer.short // block align
                    bitsPerSample = buffer.short.toInt()
                }
                "data" -> {
                    dataSize = chunkSize
                    break
                }
                else -> {
                    if (!stream.skipFully(chunkSize)) {
                        return null
                    }
                }
            }
        }

        if (sampleRate == 0 || channels == 0 || bitsPerSample == 0 || dataSize == 0L) {
            return null
        }

        val bytesPerSample = bitsPerSample / 8
        val totalSamples = dataSize / (channels * bytesPerSample)
        val durationMs = (totalSamples * 1000) / sampleRate

        return WavMetadata(
            sampleRate = sampleRate,
            channels = channels,
            bitsPerSample = bitsPerSample,
            dataSize = dataSize,
            durationMs = durationMs
        )
    }

    private fun InputStream.readFully(buffer: ByteArray): Boolean {
        var bytesRead = 0
        while (bytesRead < buffer.size) {
            val result = read(buffer, bytesRead, buffer.size - bytesRead)
            if (result < 0) {
                return false
            }
            bytesRead += result
        }
        return true
    }

    private fun InputStream.skipFully(bytesToSkip: Long): Boolean {
        var remaining = bytesToSkip
        while (remaining > 0) {
            val skipped = skip(remaining)
            if (skipped <= 0) {
                return false
            }
            remaining -= skipped
        }
        return true
    }
    
    /**
     * Create Recording object from file
     */
    fun createRecording(file: File): Recording? {
        val metadata = parseMetadata(file) ?: return null

        return Recording(
            file = file,
            documentUri = null,
            displayPath = file.absolutePath,
            fileName = file.name,
            dateTime = file.lastModified(),
            durationMs = metadata.durationMs,
            sampleRate = metadata.sampleRate,
            channels = metadata.channels,
            fileSizeBytes = file.length()
        )
    }

    fun createRecording(
        context: Context,
        documentFile: DocumentFile,
        fallbackFile: File? = null,
        displayPath: String
    ): Recording? {
        val metadata = when {
            fallbackFile != null && fallbackFile.exists() -> parseMetadata(fallbackFile)
            else -> parseMetadata(context, documentFile)
        } ?: return null

        val sizeBytes = when {
            fallbackFile != null && fallbackFile.exists() -> fallbackFile.length()
            documentFile.length() > 0 -> documentFile.length()
            else -> metadata.dataSize
        }

        val modified = when {
            fallbackFile != null && fallbackFile.exists() -> fallbackFile.lastModified()
            documentFile.lastModified() > 0 -> documentFile.lastModified()
            else -> System.currentTimeMillis()
        }

        return Recording(
            file = fallbackFile?.takeIf { it.exists() },
            documentUri = documentFile.uri,
            displayPath = displayPath,
            fileName = documentFile.name ?: fallbackFile?.name ?: "recording.wav",
            dateTime = modified,
            durationMs = metadata.durationMs,
            sampleRate = metadata.sampleRate,
            channels = metadata.channels,
            fileSizeBytes = sizeBytes
        )
    }
}
