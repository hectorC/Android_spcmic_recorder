package com.spcmic.recorder.location

import android.content.Context
import android.net.Uri
import android.util.Log
import android.util.Xml
import androidx.documentfile.provider.DocumentFile
import com.spcmic.recorder.StorageLocationManager
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.nio.charset.StandardCharsets
import java.time.Instant
import java.time.format.DateTimeFormatter
import java.util.Locale
import org.xmlpull.v1.XmlPullParser

/**
 * Manages GPX sidecar files that track recording locations.
 */
class GpxLocationRepository(private val context: Context) {

    fun readLocations(storageInfo: StorageLocationManager.StorageInfo): Map<String, RecordingLocation> {
        val input = openInputStream(storageInfo) ?: return emptyMap()
        input.use { stream ->
            return parseGpx(stream)
        }
    }

    fun upsertLocation(
        storageInfo: StorageLocationManager.StorageInfo,
        fileName: String,
        location: RecordingLocation
    ) {
        val entries = readLocations(storageInfo).toMutableMap()
        entries[fileName] = location
        writeLocations(storageInfo, entries)
    }

    fun removeLocation(
        storageInfo: StorageLocationManager.StorageInfo,
        fileName: String
    ) {
        val entries = readLocations(storageInfo).toMutableMap()
        val removed = when {
            entries.remove(fileName) != null -> true
            else -> {
                val fallbackKey = entries.keys.firstOrNull { existing ->
                    existing.equals(fileName, ignoreCase = true)
                }
                fallbackKey?.let { entries.remove(it) } != null
            }
        }

        if (!removed) {
            Log.w(TAG, "No GPX entry found for $fileName when attempting to delete")
            return
        }

        if (entries.isEmpty()) {
            deleteLocationsFile(storageInfo)
        } else {
            writeLocations(storageInfo, entries)
        }
    }

    private fun openInputStream(storageInfo: StorageLocationManager.StorageInfo): InputStream? {
        return if (storageInfo.treeUri == null) {
            val file = File(storageInfo.directory, GPX_FILE_NAME)
            if (file.exists()) file.inputStream() else null
        } else {
            val document = resolveDocument(storageInfo.treeUri)
            document?.let { context.contentResolver.openInputStream(it.uri) }
        }
    }

    private fun writeLocations(
        storageInfo: StorageLocationManager.StorageInfo,
        entries: Map<String, RecordingLocation>
    ) {
        val xmlBytes = buildGpx(entries)
        if (storageInfo.treeUri == null) {
            val file = File(storageInfo.directory, GPX_FILE_NAME)
            file.parentFile?.let { parent ->
                if (!parent.exists()) parent.mkdirs()
            }
            runCatching {
                FileOutputStream(file, false).use { stream ->
                    stream.write(xmlBytes)
                    stream.flush()
                }
            }.onFailure { throwable ->
                Log.e(TAG, "Failed to write GPX file to ${file.absolutePath}", throwable)
            }
        } else {
            val document = resolveOrCreateDocument(storageInfo.treeUri) ?: return
            context.contentResolver.openOutputStream(document.uri, "wt")?.use { stream ->
                stream.write(xmlBytes)
                stream.flush()
            } ?: Log.e(TAG, "Unable to open output stream for GPX document ${document.uri}")
        }
    }

    private fun deleteLocationsFile(storageInfo: StorageLocationManager.StorageInfo) {
        if (storageInfo.treeUri == null) {
            val file = File(storageInfo.directory, GPX_FILE_NAME)
            if (file.exists()) {
                runCatching { file.delete() }
            }
        } else {
            val document = resolveDocument(storageInfo.treeUri)
            document?.let { runCatching { it.delete() } }
        }
    }

    private fun resolveDocument(treeUri: Uri): DocumentFile? {
        val root = StorageLocationManager.getDocumentTree(context, treeUri) ?: return null
        return root.findFile(GPX_FILE_NAME)
    }

    private fun resolveOrCreateDocument(treeUri: Uri): DocumentFile? {
        val root = StorageLocationManager.getDocumentTree(context, treeUri) ?: return null
        val existing = root.findFile(GPX_FILE_NAME)
        if (existing != null && existing.isFile) {
            return existing
        }
        return root.createFile(MIME_GPX, GPX_FILE_NAME)
    }

    private fun buildGpx(entries: Map<String, RecordingLocation>): ByteArray {
        val serializer = Xml.newSerializer()
        val outputStream = ByteArrayOutputStream()
        serializer.setOutput(outputStream, StandardCharsets.UTF_8.name())
        serializer.startDocument(StandardCharsets.UTF_8.name(), true)
        serializer.setFeature("http://xmlpull.org/v1/doc/features.html#indent-output", true)
        serializer.setPrefix("", NS_GPX)
        serializer.setPrefix(SPCMIC_PREFIX, NS_SPCMIC)

        serializer.startTag(NS_GPX, "gpx")
        serializer.attribute(null, "version", "1.1")
        serializer.attribute(null, "creator", CREATOR)

        serializer.startTag(NS_GPX, "metadata")
        serializer.startTag(NS_GPX, "time")
        serializer.text(DateTimeFormatter.ISO_INSTANT.format(Instant.now()))
        serializer.endTag(NS_GPX, "time")
        serializer.endTag(NS_GPX, "metadata")

        entries.toSortedMap(String.CASE_INSENSITIVE_ORDER).forEach { (entryName, location) ->
            serializer.startTag(NS_GPX, "wpt")
            serializer.attribute(null, "lat", location.latitude.toString())
            serializer.attribute(null, "lon", location.longitude.toString())

            serializer.startTag(NS_GPX, "time")
            serializer.text(DateTimeFormatter.ISO_INSTANT.format(Instant.ofEpochMilli(location.timestampMillisUtc)))
            serializer.endTag(NS_GPX, "time")

            serializer.startTag(NS_GPX, "name")
            serializer.text(entryName)
            serializer.endTag(NS_GPX, "name")

            location.accuracyMeters?.let {
                serializer.startTag(NS_GPX, "desc")
                serializer.text(String.format(Locale.US, "Accuracy +/- %.0f m", it))
                serializer.endTag(NS_GPX, "desc")
            }

            serializer.startTag(NS_GPX, "extensions")
            location.accuracyMeters?.let {
                serializer.startTag(NS_SPCMIC, "accuracy")
                serializer.text(String.format(Locale.US, "%.2f", it))
                serializer.endTag(NS_SPCMIC, "accuracy")
            }
            location.altitudeMeters?.let {
                serializer.startTag(NS_SPCMIC, "altitude")
                serializer.text(String.format(Locale.US, "%.2f", it))
                serializer.endTag(NS_SPCMIC, "altitude")
            }
            location.provider?.let {
                serializer.startTag(NS_SPCMIC, "provider")
                serializer.text(it)
                serializer.endTag(NS_SPCMIC, "provider")
            }
            serializer.endTag(NS_GPX, "extensions")

            serializer.endTag(NS_GPX, "wpt")
        }

        serializer.endTag(NS_GPX, "gpx")
        serializer.endDocument()
        serializer.flush()
        return outputStream.toByteArray()
    }

    private fun parseGpx(stream: InputStream): Map<String, RecordingLocation> {
        val parser = Xml.newPullParser()
        parser.setFeature(XmlPullParser.FEATURE_PROCESS_NAMESPACES, true)
        parser.setInput(stream, StandardCharsets.UTF_8.name())

        val results = mutableMapOf<String, RecordingLocation>()
        var event = parser.eventType
        while (event != XmlPullParser.END_DOCUMENT) {
            if (event == XmlPullParser.START_TAG && parser.name == "wpt") {
                parseWaypoint(parser)?.let { (name, location) ->
                    results[name] = location
                }
            }
            event = parser.next()
        }
        return results
    }

    private fun parseWaypoint(parser: XmlPullParser): Pair<String, RecordingLocation>? {
        val latAttr = parser.getAttributeValue(null, "lat")?.toDoubleOrNull()
        val lonAttr = parser.getAttributeValue(null, "lon")?.toDoubleOrNull()
        if (latAttr == null || lonAttr == null) {
            skipTag(parser)
            return null
        }

        var name: String? = null
        var time = System.currentTimeMillis()
        var accuracy: Float? = null
        var altitude: Double? = null
        var provider: String? = null

        var event = parser.next()
        while (!(event == XmlPullParser.END_TAG && parser.name == "wpt")) {
            if (event == XmlPullParser.START_TAG) {
                when (parser.name) {
                    "name" -> name = parser.nextText()
                    "time" -> {
                        val text = parser.nextText()
                        time = runCatching { Instant.parse(text).toEpochMilli() }
                            .getOrDefault(time)
                    }
                    "extensions" -> {
                        parseExtensions(parser)?.let { ext ->
                            accuracy = ext.accuracyMeters ?: accuracy
                            altitude = ext.altitudeMeters ?: altitude
                            provider = ext.provider ?: provider
                        }
                    }
                    "desc" -> parser.nextText()
                }
            }
            event = parser.next()
        }

        val waypointName = name ?: return null
        val location = RecordingLocation(
            latitude = latAttr,
            longitude = lonAttr,
            accuracyMeters = accuracy,
            altitudeMeters = altitude,
            timestampMillisUtc = time,
            provider = provider
        )
        return waypointName to location
    }

    private fun parseExtensions(parser: XmlPullParser): ExtensionData? {
        var accuracy: Float? = null
        var altitude: Double? = null
        var provider: String? = null

        var event = parser.next()
        while (!(event == XmlPullParser.END_TAG && parser.name == "extensions")) {
            if (event == XmlPullParser.START_TAG && parser.namespace == NS_SPCMIC) {
                when (parser.name) {
                    "accuracy" -> accuracy = parser.nextText().toFloatOrNull()
                    "altitude" -> altitude = parser.nextText().toDoubleOrNull()
                    "provider" -> provider = parser.nextText()
                    else -> skipTag(parser)
                }
            } else if (event == XmlPullParser.START_TAG) {
                skipTag(parser)
            }
            event = parser.next()
        }

        return ExtensionData(accuracy, altitude, provider)
    }

    private fun skipTag(parser: XmlPullParser) {
        var depth = 1
        while (depth > 0) {
            when (parser.next()) {
                XmlPullParser.END_TAG -> depth--
                XmlPullParser.START_TAG -> depth++
            }
        }
    }

    private data class ExtensionData(
        val accuracyMeters: Float?,
        val altitudeMeters: Double?,
        val provider: String?
    )

    companion object {
        private const val GPX_FILE_NAME = "spcmic_locations.gpx"
        private const val MIME_GPX = "application/gpx+xml"
        private const val CREATOR = "spcmic-recorder"
        private const val NS_GPX = "http://www.topografix.com/GPX/1/1"
        private const val NS_SPCMIC = "urn:spcmic:1.0"
        private const val SPCMIC_PREFIX = "spcmic"
        private const val TAG = "GpxLocationRepo"
    }
}
