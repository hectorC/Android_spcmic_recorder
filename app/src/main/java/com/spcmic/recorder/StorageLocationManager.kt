package com.spcmic.recorder

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.os.storage.StorageManager
import android.provider.DocumentsContract
import android.util.Log
import androidx.documentfile.provider.DocumentFile
import java.io.File

object StorageLocationManager {
    const val PERMISSION_FLAGS = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    private const val PREFS_NAME = "storage_location_prefs"
    private const val KEY_PATH = "recording_path"
    private const val KEY_TREE_URI = "recording_tree_uri"
    private const val DEFAULT_DIR_NAME = "SPCMicRecorder"
    private const val MIME_TYPE_WAV = "audio/wav"
    private const val TAG = "StorageLocationMgr"

    data class StorageInfo(val directory: File, val displayPath: String, val treeUri: Uri?)

    data class RecordingTarget(
        val outputFile: File?,
        val parcelFileDescriptor: ParcelFileDescriptor?,
        val documentUri: Uri?,
        val displayLocation: String,
        val absolutePath: String?
    )

    fun getStorageInfo(context: Context): StorageInfo {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val storedPath = prefs.getString(KEY_PATH, null)
        val directory = if (!storedPath.isNullOrEmpty()) {
            File(storedPath)
        } else {
            val defaultDir = File(
                Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS),
                DEFAULT_DIR_NAME
            )
            prefs.edit().putString(KEY_PATH, defaultDir.absolutePath).apply()
            defaultDir
        }

        if (!directory.exists()) {
            directory.mkdirs()
        }

        val treeUri = prefs.getString(KEY_TREE_URI, null)?.let { Uri.parse(it) }
        return StorageInfo(directory, directory.absolutePath, treeUri)
    }

    fun updateStorageInfo(context: Context, treeUri: Uri): StorageInfo? {
        val document = DocumentFile.fromTreeUri(context, treeUri)?.takeIf { it.exists() && it.isDirectory }
        if (document == null) {
            Log.e(TAG, "Selected tree uri is not a directory or accessible: $treeUri")
            return null
        }

        val targetPath = documentUriToAbsolutePath(context, treeUri) ?: run {
            Log.e(TAG, "Unable to resolve path for uri: $treeUri")
            return null
        }
        val directory = File(targetPath)
        if (!directory.exists()) {
            if (!directory.mkdirs()) {
                // Directory might already exist but File API cannot see it; rely on SAF permissions instead
                Log.w(TAG, "Directory $targetPath not directly accessible via File API; continuing with SAF access")
            }
        }

        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        prefs.edit()
            .putString(KEY_PATH, directory.absolutePath)
            .putString(KEY_TREE_URI, treeUri.toString())
            .apply()

        return StorageInfo(directory, directory.absolutePath, treeUri)
    }

    fun ensureExportsDirectory(baseDirectory: File): File {
        val exportsDir = File(baseDirectory, "Exports")
        if (!exportsDir.exists()) {
            exportsDir.mkdirs()
        }
        return exportsDir
    }

    fun ensureExportsDocumentDirectory(context: Context, treeUri: Uri): DocumentFile? {
        val root = getDocumentTree(context, treeUri) ?: return null
        val existing = root.findFile("Exports")
        val exports = when {
            existing == null -> root.createDirectory("Exports")
            existing.isDirectory -> existing
            else -> {
                runCatching { existing.delete() }
                root.createDirectory("Exports")
            }
        }
        return exports?.takeIf { it.isDirectory && it.exists() }
    }

    fun createOrReplaceDocumentFile(directory: DocumentFile, fileName: String, mimeType: String = MIME_TYPE_WAV): DocumentFile? {
        directory.findFile(fileName)?.let { existing ->
            if (existing.isFile) {
                runCatching { existing.delete() }
            }
        }
        return directory.createFile(mimeType, fileName)
    }

    fun prepareRecordingTarget(context: Context, fileName: String): RecordingTarget? {
        val info = getStorageInfo(context)

        if (info.treeUri == null) {
            val directory = info.directory
            if (!directory.exists()) {
                directory.mkdirs()
            }
            val file = File(directory, fileName)
            return RecordingTarget(
                outputFile = file,
                parcelFileDescriptor = null,
                documentUri = null,
                displayLocation = file.absolutePath,
                absolutePath = file.absolutePath
            )
        }

        val baseDocument = DocumentFile.fromTreeUri(context, info.treeUri)
        if (baseDocument == null || !baseDocument.isDirectory) {
            Log.e(TAG, "Unable to resolve DocumentFile for uri: ${info.treeUri}")
            return null
        }

        // Remove any stale file with the same name before creating a new one
        runCatching {
            baseDocument.findFile(fileName)?.delete()
        }

        val newDocument = baseDocument.createFile(MIME_TYPE_WAV, fileName)
        if (newDocument == null) {
            Log.e(TAG, "Failed to create DocumentFile for recording: $fileName")
            return null
        }

        val parcelFileDescriptor = try {
            context.contentResolver.openFileDescriptor(newDocument.uri, "rw")
        } catch (e: Exception) {
            Log.e(TAG, "Failed to open ParcelFileDescriptor for ${newDocument.uri}", e)
            null
        }

        if (parcelFileDescriptor == null) {
            runCatching { newDocument.delete() }
            return null
        }

        val absolutePath = documentUriToAbsolutePath(context, newDocument.uri)
        val displayLocation = absolutePath ?: newDocument.uri.toString()

        return RecordingTarget(
            outputFile = null,
            parcelFileDescriptor = parcelFileDescriptor,
            documentUri = newDocument.uri,
            displayLocation = displayLocation,
            absolutePath = absolutePath
        )
    }

    fun getDocumentTree(context: Context, treeUri: Uri?): DocumentFile? {
        if (treeUri == null) return null
        val flags = PERMISSION_FLAGS
        try {
            context.contentResolver.takePersistableUriPermission(treeUri, flags)
        } catch (_: SecurityException) {
            // Already granted or not persistable
        }
        return DocumentFile.fromTreeUri(context, treeUri)
    }

    fun documentUriToAbsolutePath(context: Context, documentUri: Uri): String? {
        val documentId = try {
            DocumentsContract.getDocumentId(documentUri)
        } catch (e: IllegalArgumentException) {
            if (DocumentsContract.isTreeUri(documentUri)) {
                try {
                    DocumentsContract.getTreeDocumentId(documentUri)
                } catch (inner: IllegalArgumentException) {
                    Log.e(TAG, "Unable to extract document ID from $documentUri", inner)
                    return null
                }
            } else {
                Log.e(TAG, "Unable to extract document ID from $documentUri", e)
                return null
            }
        }

        val colonIndex = documentId.indexOf(':')
        val volumeId = if (colonIndex == -1) documentId else documentId.substring(0, colonIndex)
        val relativePath = if (colonIndex == -1 || colonIndex == documentId.length - 1) {
            ""
        } else {
            documentId.substring(colonIndex + 1)
        }

        val storageManager = context.getSystemService(StorageManager::class.java)
        val storageVolumes = storageManager?.storageVolumes ?: emptyList()

        val baseDir: File = when {
            volumeId.equals("primary", ignoreCase = true) || volumeId.isEmpty() -> Environment.getExternalStorageDirectory()
            else -> storageVolumes.firstOrNull { it.uuid?.equals(volumeId, ignoreCase = true) == true }?.directory
        } ?: File("/storage/$volumeId")

        val basePath = baseDir.absolutePath
        return if (relativePath.isEmpty()) basePath else File(basePath, relativePath).path
    }
}
