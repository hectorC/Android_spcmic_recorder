package com.spcmic.recorder.playback

import android.util.Log
import java.io.File
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
    
    /**
     * Create Recording object from file
     */
    fun createRecording(file: File): Recording? {
        val metadata = parseMetadata(file) ?: return null
        
        return Recording(
            file = file,
            fileName = file.name,
            dateTime = file.lastModified(),
            durationMs = metadata.durationMs,
            sampleRate = metadata.sampleRate,
            channels = metadata.channels,
            fileSizeBytes = file.length()
        )
    }
}
