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
        val headerDataSize: Long,
        val dataOffset: Long,
        val dataSizeBytes: Long,
        val sampleCount: Long?,
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
                val isRf64 = riff == "RF64"
                if (!isRf64 && riff != "RIFF") {
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
                var headerDataSize = 0L
                var dataChunkOffset = -1L
                var ds64DataSize: Long? = null
                var ds64SampleCount: Long? = null
                
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
                            headerDataSize = chunkSize
                            dataChunkOffset = raf.filePointer
                            break // We have all we need
                        }
                        "ds64" -> {
                            if (chunkSize > Int.MAX_VALUE) {
                                Log.e(TAG, "ds64 chunk too large to process: $chunkSize bytes")
                                return null
                            }
                            val size = chunkSize.toInt()
                            val ds64Data = ByteArray(size)
                            raf.readFully(ds64Data)

                            if (size >= 28) {
                                val buffer = ByteBuffer.wrap(ds64Data).order(ByteOrder.LITTLE_ENDIAN)
                                buffer.long // riffSize64 (unused)
                                ds64DataSize = buffer.long
                                ds64SampleCount = buffer.long
                                buffer.int // tableLength (ignored)
                            }
                        }
                        else -> {
                            // Skip unknown chunk
                            raf.seek(raf.filePointer + chunkSize)
                        }
                    }

                    if (chunkSize % 2L == 1L) {
                        raf.seek(raf.filePointer + 1)
                    }
                }
                
                if (sampleRate == 0 || channels == 0 || bitsPerSample == 0 || headerDataSize == 0L || dataChunkOffset < 0L) {
                    Log.e(TAG, "Invalid WAV metadata for file: $file")
                    return null
                }
                
                // Calculate duration (account for files larger than 4 GB where header wraps)
                val bytesPerSample = bitsPerSample / 8
                if (bytesPerSample <= 0) {
                    Log.e(TAG, "Invalid bytes-per-sample for file: $file")
                    return null
                }

                val fileLength = raf.length()
                val frameSize = channels * bytesPerSample
                val availablePayload = if (fileLength > dataChunkOffset) fileLength - dataChunkOffset else 0L
                val dataSize = ds64DataSize?.takeIf { it > 0L } ?: when {
                    availablePayload > headerDataSize && availablePayload > 0L -> availablePayload
                    headerDataSize == 0L && availablePayload > 0L -> availablePayload
                    headerDataSize > 0L && headerDataSize != 0xFFFFFFFFL -> headerDataSize
                    availablePayload > 0L -> availablePayload
                    else -> headerDataSize
                }
                val totalFrames = when {
                    frameSize > 0 -> dataSize / frameSize
                    else -> 0L
                }
                val resolvedSampleCount = ds64SampleCount?.takeIf { it > 0L } ?: totalFrames.takeIf { it > 0L }
                val durationMs = if (sampleRate > 0 && totalFrames > 0) (totalFrames * 1000L) / sampleRate else 0L
                
                return WavMetadata(
                    sampleRate = sampleRate,
                    channels = channels,
                    bitsPerSample = bitsPerSample,
                    headerDataSize = headerDataSize,
                    dataOffset = dataChunkOffset,
                    dataSizeBytes = dataSize,
                    sampleCount = resolvedSampleCount,
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
        if (riff != "RIFF" && riff != "RF64") {
            return null
        }

        val wave = String(header, 8, 4)
        if (wave != "WAVE") {
            return null
        }
        var sampleRate = 0
        var channels = 0
        var bitsPerSample = 0
        var headerDataSize = 0L
        var dataOffset = -1L
        var bytesConsumed = 12L
        var ds64DataSize: Long? = null
        var ds64SampleCount: Long? = null

        val chunkHeader = ByteArray(8)

        while (stream.readFully(chunkHeader)) {
            val chunkId = String(chunkHeader, 0, 4)
            val chunkSize = ByteBuffer.wrap(chunkHeader, 4, 4)
                .order(ByteOrder.LITTLE_ENDIAN)
                .int
                .toLong() and 0xFFFFFFFFL
            bytesConsumed += 8

            when (chunkId) {
                "fmt " -> {
                    val fmtData = ByteArray(chunkSize.toInt())
                    if (!stream.readFully(fmtData)) {
                        return null
                    }
                    bytesConsumed += chunkSize

                    val buffer = ByteBuffer.wrap(fmtData).order(ByteOrder.LITTLE_ENDIAN)
                    buffer.short // audio format
                    channels = buffer.short.toInt()
                    sampleRate = buffer.int
                    buffer.int // byte rate
                    buffer.short // block align
                    bitsPerSample = buffer.short.toInt()
                }
                "ds64" -> {
                    if (chunkSize > Int.MAX_VALUE) {
                        return null
                    }
                    val size = chunkSize.toInt()
                    val ds64Data = ByteArray(size)
                    if (!stream.readFully(ds64Data)) {
                        return null
                    }
                    bytesConsumed += chunkSize

                    if (size >= 28) {
                        val buffer = ByteBuffer.wrap(ds64Data).order(ByteOrder.LITTLE_ENDIAN)
                        buffer.long // riffSize64 (unused)
                        ds64DataSize = buffer.long
                        ds64SampleCount = buffer.long
                        buffer.int // tableLength (ignored)
                    }
                }
                "data" -> {
                    headerDataSize = chunkSize
                    dataOffset = bytesConsumed
                    break
                }
                else -> {
                    if (!stream.skipFully(chunkSize)) {
                        return null
                    }
                    bytesConsumed += chunkSize
                }
            }

            if (chunkSize % 2L == 1L) {
                if (!stream.skipFully(1)) {
                    return null
                }
                bytesConsumed += 1
            }
        }

        if (sampleRate == 0 || channels == 0 || bitsPerSample == 0 || headerDataSize == 0L || dataOffset < 0L) {
            return null
        }

        val bytesPerSample = bitsPerSample / 8
        if (bytesPerSample <= 0) {
            return null
        }
        val dataSize = ds64DataSize?.takeIf { it > 0L } ?: headerDataSize
        val resolvedSampleCount = ds64SampleCount?.takeIf { it > 0L }
        val durationMs = if (sampleRate > 0 && resolvedSampleCount != null) (resolvedSampleCount * 1000L) / sampleRate else 0L

        return WavMetadata(
            sampleRate = sampleRate,
            channels = channels,
            bitsPerSample = bitsPerSample,
            headerDataSize = headerDataSize,
            dataOffset = dataOffset,
            dataSizeBytes = dataSize,
            sampleCount = resolvedSampleCount,
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

    private fun resolveDuration(metadata: WavMetadata, totalFileSize: Long?): Long {
        val sampleCount = metadata.sampleCount
        if (sampleCount != null && sampleCount > 0 && metadata.sampleRate > 0) {
            return (sampleCount * 1000L) / metadata.sampleRate
        }

        val bytesPerSample = metadata.bitsPerSample / 8
        if (bytesPerSample <= 0) {
            return 0L
        }

        var candidateSize = metadata.dataSizeBytes.takeIf { it > 0 } ?: metadata.headerDataSize
        if (totalFileSize != null && totalFileSize > 0 && metadata.dataOffset >= 0) {
            val payload = totalFileSize - metadata.dataOffset
            if (payload > candidateSize) {
                candidateSize = payload
            }
        }

        if (candidateSize <= 0L) {
            return 0L
        }

        val frameSize = metadata.channels * bytesPerSample
        if (frameSize <= 0) {
            return 0L
        }

        val totalFrames = candidateSize / frameSize
        return if (metadata.sampleRate > 0) (totalFrames * 1000L) / metadata.sampleRate else 0L
    }

    /**
     * Create Recording object from file
     */
    fun createRecording(file: File): Recording? {
        val metadata = parseMetadata(file) ?: return null

        val durationMs = resolveDuration(metadata, file.length())

        return Recording(
            file = file,
            documentUri = null,
            displayPath = file.absolutePath,
            fileName = file.name,
            dateTime = file.lastModified(),
            durationMs = durationMs,
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
            else -> (metadata.dataOffset + metadata.dataSizeBytes).takeIf { it > 0 } ?: metadata.headerDataSize
        }

        val modified = when {
            fallbackFile != null && fallbackFile.exists() -> fallbackFile.lastModified()
            documentFile.lastModified() > 0 -> documentFile.lastModified()
            else -> System.currentTimeMillis()
        }

        val durationMs = resolveDuration(metadata, sizeBytes.takeIf { it > 0 })

        return Recording(
            file = fallbackFile?.takeIf { it.exists() },
            documentUri = documentFile.uri,
            displayPath = displayPath,
            fileName = documentFile.name ?: fallbackFile?.name ?: "recording.wav",
            dateTime = modified,
            durationMs = durationMs,
            sampleRate = metadata.sampleRate,
            channels = metadata.channels,
            fileSizeBytes = sizeBytes
        )
    }
}
