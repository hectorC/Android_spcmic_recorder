package com.spcmic.recorder

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Environment
import android.provider.DocumentsContract
import android.util.Log
import java.io.File

object StorageLocationManager {
    const val PERMISSION_FLAGS = Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
    private const val PREFS_NAME = "storage_location_prefs"
    private const val KEY_PATH = "recording_path"
    private const val KEY_TREE_URI = "recording_tree_uri"
    private const val DEFAULT_DIR_NAME = "SPCMicRecorder"
    private const val TAG = "StorageLocationMgr"

    data class StorageInfo(val directory: File, val displayPath: String, val treeUri: Uri?)

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
        val targetPath = treeUriToAbsolutePath(treeUri) ?: run {
            Log.e(TAG, "Unable to resolve path for uri: $treeUri")
            return null
        }
        val directory = File(targetPath)
        if (!directory.exists() && !directory.mkdirs()) {
            Log.e(TAG, "Failed to create target directory at $targetPath")
            return null
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

    private fun treeUriToAbsolutePath(treeUri: Uri): String? {
        val documentId = DocumentsContract.getTreeDocumentId(treeUri)
        val colonIndex = documentId.indexOf(':')
        val volumeId = if (colonIndex == -1) documentId else documentId.substring(0, colonIndex)
        val relativePath = if (colonIndex == -1 || colonIndex == documentId.length - 1) {
            ""
        } else {
            documentId.substring(colonIndex + 1)
        }

        val basePath = when (volumeId) {
            "primary" -> Environment.getExternalStorageDirectory().absolutePath
            else -> "/storage/$volumeId"
        }

        return if (relativePath.isEmpty()) {
            basePath
        } else {
            "$basePath/$relativePath"
        }
    }
}
