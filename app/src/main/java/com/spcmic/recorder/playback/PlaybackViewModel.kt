package com.spcmic.recorder.playback

import android.content.Context
import android.content.SharedPreferences
import android.content.res.AssetManager
import android.util.Log
import androidx.annotation.StringRes
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.spcmic.recorder.R
import com.spcmic.recorder.StorageLocationManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File

/**
 * ViewModel for playback screen that supports both file-system and SAF storage.
 */
class PlaybackViewModel : ViewModel() {
    companion object {
        private const val TAG = "PlaybackViewModel"
        const val PREFS_NAME = "playback_cache_prefs"
        private const val PREF_LAST_CACHE_SOURCE = "last_cached_source"
        private const val CACHE_FILE_NAME = "playback_cache.wav"
    }

    private sealed interface ExportOutcome {
        data class Local(val file: File) : ExportOutcome
        data class Document(val document: DocumentFile) : ExportOutcome
    }

    private val _recordings = MutableLiveData<List<Recording>>(emptyList())
    val recordings: LiveData<List<Recording>> = _recordings

    private val _isLoading = MutableLiveData(false)
    val isLoading: LiveData<Boolean> = _isLoading

    private val _selectedRecording = MutableLiveData<Recording?>()
    val selectedRecording: LiveData<Recording?> = _selectedRecording

    private val _selectedIRPreset = MutableLiveData(IRPreset.BINAURAL)
    val selectedIRPreset: LiveData<IRPreset> = _selectedIRPreset

    private val _isPlaying = MutableLiveData(false)
    val isPlaying: LiveData<Boolean> = _isPlaying

    private val _currentPosition = MutableLiveData(0L)
    val currentPosition: LiveData<Long> = _currentPosition

    private val _totalDuration = MutableLiveData(0L)
    val totalDuration: LiveData<Long> = _totalDuration

    private val _isProcessing = MutableLiveData(false)
    val isProcessing: LiveData<Boolean> = _isProcessing

    private val _processingProgress = MutableLiveData(0)
    val processingProgress: LiveData<Int> = _processingProgress

    private val _processingMessage = MutableLiveData(R.string.preprocessing_message)
    val processingMessage: LiveData<Int> = _processingMessage

    private val _statusMessage = MutableLiveData<PlaybackMessage?>()
    val statusMessage: LiveData<PlaybackMessage?> = _statusMessage

    private val _playbackGainDb = MutableLiveData(0f)
    val playbackGainDb: LiveData<Float> = _playbackGainDb

    private val _isLooping = MutableLiveData(false)
    val isLooping: LiveData<Boolean> = _isLooping

    private val _storagePath = MutableLiveData<String>()
    val storagePath: LiveData<String> = _storagePath

    private var playbackEngine: NativePlaybackEngine? = null
    private var positionUpdateJob: Job? = null
    private var assetManager: AssetManager? = null
    private var cacheDirectory: String? = null
    private var preferences: SharedPreferences? = null
    private val preprocessMutex = Mutex()

    private var appContext: Context? = null
    private var storageInfo: StorageLocationManager.StorageInfo? = null

    fun attachContext(context: Context) {
        appContext = context.applicationContext
    }

    fun setAssetManager(manager: AssetManager) {
        assetManager = manager
        playbackEngine?.setAssetManager(manager)
    }

    fun setCacheDirectory(path: String) {
        cacheDirectory = path
        playbackEngine?.setCacheDirectory(path)
    }

    fun setPreferences(prefs: SharedPreferences) {
        preferences = prefs
    }

    fun updateStorageLocation(info: StorageLocationManager.StorageInfo) {
        storageInfo = info
        _storagePath.value = info.displayPath
    }

    fun scanRecordings() {
        viewModelScope.launch {
            _isLoading.value = true
            val info = storageInfo
            val context = appContext
            
            Log.d(TAG, "scanRecordings: Starting scan. info=$info, context=$context")
            
            val recordingList = if (info == null) {
                Log.w(TAG, "scanRecordings: No storage info available")
                emptyList()
            } else {
                try {
                    withContext(Dispatchers.IO) {
                        if (info.treeUri == null) {
                            Log.d(TAG, "scanRecordings: Scanning file directory: ${info.directory.absolutePath}")
                            scanFileDirectory(info.directory)
                        } else {
                            Log.d(TAG, "scanRecordings: Scanning document tree: ${info.treeUri}")
                            if (context == null) {
                                Log.e(TAG, "scanRecordings: Context is null for document tree scan")
                                emptyList()
                            } else {
                                scanDocumentTree(context, info)
                            }
                        }
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "scanRecordings: Error scanning recordings", e)
                    emptyList()
                }
            }

            Log.d(TAG, "scanRecordings: Found ${recordingList.size} recordings")
            _recordings.value = recordingList
            _isLoading.value = false
        }
    }

    private fun scanFileDirectory(directory: File): List<Recording> {
        Log.d(TAG, "scanFileDirectory: Scanning ${directory.absolutePath}")
        Log.d(TAG, "scanFileDirectory: Exists=${directory.exists()}, IsDirectory=${directory.isDirectory}")
        
        if (!directory.exists()) {
            Log.w(TAG, "scanFileDirectory: Directory doesn't exist, creating it")
            directory.mkdirs()
        }

        val files = directory.listFiles()
        Log.d(TAG, "scanFileDirectory: listFiles() returned ${files?.size ?: 0} files")

        val wavFiles = files
            ?.filter { file -> file.isFile && file.name.endsWith(".wav", ignoreCase = true) }
            ?: emptyList()

        Log.d(TAG, "scanFileDirectory: Found ${wavFiles.size} WAV files")

        val recordings = wavFiles.mapNotNull { file ->
            try {
                WavMetadataParser.createRecording(file)
            } catch (e: Exception) {
                Log.e(TAG, "scanFileDirectory: Error parsing ${file.name}", e)
                null
            }
        }.sortedByDescending { it.dateTime }
            
        Log.d(TAG, "scanFileDirectory: Returning ${recordings.size} recordings")
        return recordings
    }

    private fun scanDocumentTree(context: Context, info: StorageLocationManager.StorageInfo): List<Recording> {
        Log.d(TAG, "scanDocumentTree: Scanning tree ${info.treeUri}")
        
        val root = StorageLocationManager.getDocumentTree(context, info.treeUri)
        if (root == null) {
            Log.e(TAG, "scanDocumentTree: Failed to get document tree for ${info.treeUri}")
            return emptyList()
        }
        
        Log.d(TAG, "scanDocumentTree: Got root document: ${root.uri}")
        
        val documents = try {
            root.listFiles()
        } catch (e: Exception) {
            Log.e(TAG, "scanDocumentTree: Error listing files in document tree", e)
            return emptyList()
        }
        
        Log.d(TAG, "scanDocumentTree: listFiles() returned ${documents.size} documents")

        val recordings = documents.asSequence()
            .filter { doc -> 
                val isWav = doc.isFile && (doc.name?.endsWith(".wav", ignoreCase = true) == true)
                if (isWav) Log.d(TAG, "scanDocumentTree: Found WAV file: ${doc.name}")
                isWav
            }
            .mapNotNull { doc ->
                try {
                    val absolutePath = StorageLocationManager.documentUriToAbsolutePath(context, doc.uri)
                    val fallbackFile = absolutePath?.let { File(it) }?.takeIf { it.exists() }
                    val displayPath = absolutePath ?: doc.uri.toString()
                    WavMetadataParser.createRecording(
                        context = context,
                        documentFile = doc,
                        fallbackFile = fallbackFile,
                        displayPath = displayPath
                    )
                } catch (e: Exception) {
                    Log.e(TAG, "scanDocumentTree: Error parsing ${doc.name}", e)
                    null
                }
            }
            .sortedByDescending { it.dateTime }
            .toList()
            
        Log.d(TAG, "scanDocumentTree: Returning ${recordings.size} recordings")
        return recordings
    }

    fun selectRecording(recording: Recording) {
        stopPlayback()

        viewModelScope.launch(Dispatchers.IO) {
            try {
                val engine = playbackEngine ?: NativePlaybackEngine().also { playbackEngine = it }
                assetManager?.let { engine.setAssetManager(it) }
                cacheDirectory?.let { engine.setCacheDirectory(it) }
                engine.setPlaybackGain(_playbackGainDb.value ?: 0f)
                engine.setLooping(_isLooping.value ?: false)

                val loaded = loadRecordingIntoEngine(recording)
                if (loaded) {
                    val reused = tryReuseCachedPreRender(recording)
                    val durationMs = (engine.getDuration() * 1000).toLong()

                    withContext(Dispatchers.Main) {
                        _selectedRecording.value = recording
                        _totalDuration.value = durationMs
                        _currentPosition.value = 0L
                        _isPlaying.value = false
                        _isProcessing.value = false
                        _processingProgress.value = if (reused) 100 else 0
                        _processingMessage.value = R.string.preprocessing_message
                        _statusMessage.value = if (reused) {
                            PlaybackMessage(R.string.playback_cached_ready)
                        } else {
                            null
                        }
                    }
                } else {
                    withContext(Dispatchers.Main) {
                        _statusMessage.value = PlaybackMessage(R.string.playback_not_ready)
                    }
                }
            } catch (t: Throwable) {
                Log.e(TAG, "Error loading recording ", t)
                withContext(Dispatchers.Main) {
                    _statusMessage.value = PlaybackMessage(R.string.playback_not_ready)
                }
            }
        }
    }

    private fun loadRecordingIntoEngine(recording: Recording): Boolean {
        val engine = playbackEngine ?: return false
        val file = recording.file
        if (file != null && file.exists()) {
            return engine.loadFile(file.absolutePath)
        }

        val uri = recording.documentUri ?: return false
        val context = appContext ?: return false

        return runCatching {
            context.contentResolver.openFileDescriptor(uri, "r")?.use { pfd ->
                val fd = pfd.detachFd()
                engine.loadFileFromDescriptor(fd, recording.displayPath)
            } ?: false
        }.onFailure { throwable ->
            Log.e(TAG, "Failed to load recording from SAF: ", throwable)
        }.getOrDefault(false)
    }

    fun clearSelection() {
        stopPlayback()
        playbackEngine?.release()
        playbackEngine = null

        _selectedRecording.value = null
        _isPlaying.value = false
        _currentPosition.value = 0L
        _totalDuration.value = 0L
        _isProcessing.value = false
        _processingProgress.value = 0
        _processingMessage.value = R.string.preprocessing_message
    }

    fun setPlaybackGain(gainDb: Float) {
        val clamped = gainDb.coerceIn(0f, 48f)
        _playbackGainDb.value = clamped
        playbackEngine?.setPlaybackGain(clamped)
    }

    fun setLooping(looping: Boolean) {
        _isLooping.value = looping
        playbackEngine?.setLooping(looping)
    }

    fun play() {
        val engine = playbackEngine ?: return

        if (engine.isPreRenderReady()) {
            if (engine.play()) {
                _isPlaying.value = true
                startPositionUpdates()
            } else {
                _statusMessage.value = PlaybackMessage(R.string.playback_not_ready)
            }
            return
        }

        viewModelScope.launch {
            val ready = ensurePreprocessed()
            if (!ready) {
                return@launch
            }

            if (engine.play()) {
                _isPlaying.value = true
                _currentPosition.value = 0L
                startPositionUpdates()
            } else {
                _statusMessage.value = PlaybackMessage(R.string.playback_not_ready)
            }
        }
    }

    fun pause() {
        playbackEngine?.pause()
        _isPlaying.value = false
        stopPositionUpdates()
    }

    fun stopPlayback() {
        playbackEngine?.stop()
        _isPlaying.value = false
        _currentPosition.value = 0L
        stopPositionUpdates()
    }

    fun seekTo(positionMs: Long) {
        val positionSeconds = positionMs / 1000.0
        playbackEngine?.seek(positionSeconds)
        _currentPosition.value = positionMs
    }

    fun exportRecording(recording: Recording) {
        viewModelScope.launch {
            val assets = assetManager
            val cacheDir = cacheDirectory
            val context = appContext
            val info = storageInfo

            if (assets == null || cacheDir == null || context == null) {
                _statusMessage.value = PlaybackMessage(R.string.export_failure)
                return@launch
            }

            preprocessMutex.withLock {
                _processingMessage.value = R.string.exporting_message
                _processingProgress.value = 0
                _isProcessing.value = true

                var success = false
                var outcome: ExportOutcome? = null

                try {
                    val cachedPreRender = playbackCacheForRecording(recording)
                    var reuseSucceeded = false

                    if (cachedPreRender != null) {
                        outcome = withContext(Dispatchers.IO) {
                            reuseCachedPreRenderForExport(cachedPreRender, recording, context, info)
                        }
                        reuseSucceeded = outcome != null
                        if (reuseSucceeded) {
                            _processingProgress.value = 100
                            success = true
                        } else {
                            _processingProgress.value = 0
                        }
                    }

                    if (!reuseSucceeded) {
                        val engine = NativePlaybackEngine()
                        try {
                            engine.setAssetManager(assets)
                            engine.setCacheDirectory(cacheDir)

                            val loadSuccess = withContext(Dispatchers.IO) {
                                when {
                                    recording.file?.exists() == true -> engine.loadFile(recording.file.absolutePath)
                                    recording.documentUri != null -> context.contentResolver.openFileDescriptor(recording.documentUri, "r")?.use { pfd ->
                                        val fd = pfd.detachFd()
                                        engine.loadFileFromDescriptor(fd, recording.displayPath)
                                    } ?: false
                                    else -> false
                                }
                            }

                            if (!loadSuccess) {
                                _statusMessage.value = PlaybackMessage(R.string.export_failure)
                                return@withLock
                            }

                            success = coroutineScope {
                                val prepareDeferred = async(Dispatchers.IO) { engine.preparePreRender() }
                                val progressJob = launch {
                                    while (isActive) {
                                        val progress = engine.getPreRenderProgress().coerceIn(0, 100)
                                        _processingProgress.value = progress
                                        if (prepareDeferred.isCompleted && progress >= 100) {
                                            break
                                        }
                                        delay(150)
                                    }
                                }

                                val prepared = prepareDeferred.await()
                                progressJob.cancelAndJoin()

                                if (!prepared) {
                                    _processingProgress.value = 0
                                    return@coroutineScope false
                                }

                                _processingProgress.value = 100

                                outcome = withContext(Dispatchers.IO) {
                                    when {
                                        recording.file?.exists() == true -> {
                                            val recordingDir = recording.file.parentFile ?: return@withContext null
                                            val exportDir = StorageLocationManager.ensureExportsDirectory(recordingDir)
                                            val exportFile = File(exportDir, buildExportFileName(recording))
                                            if (engine.exportPreRendered(exportFile.absolutePath)) {
                                                ExportOutcome.Local(exportFile)
                                            } else {
                                                null
                                            }
                                        }
                                        recording.documentUri != null -> {
                                            val storage = info
                                            val treeUri = storage?.treeUri ?: return@withContext null
                                            val exportsDir = StorageLocationManager.ensureExportsDocumentDirectory(context, treeUri)
                                                ?: return@withContext null
                                            val targetName = buildExportFileName(recording)
                                            val tempFile = File(cacheDir, "export_temp_.wav")
                                            if (tempFile.exists()) {
                                                tempFile.delete()
                                            }

                                            if (!engine.exportPreRendered(tempFile.absolutePath)) {
                                                tempFile.delete()
                                                return@withContext null
                                            }

                                            val document = StorageLocationManager.createOrReplaceDocumentFile(exportsDir, targetName)
                                                ?: run {
                                                    tempFile.delete()
                                                    return@withContext null
                                                }

                                            val copied = context.contentResolver.openOutputStream(document.uri, "w")?.use { output ->
                                                tempFile.inputStream().use { input -> input.copyTo(output) }
                                                true
                                            } ?: false

                                            tempFile.delete()

                                            if (copied) {
                                                ExportOutcome.Document(document)
                                            } else {
                                                runCatching { document.delete() }
                                                null
                                            }
                                        }
                                        else -> null
                                    }
                                }

                                outcome != null
                            }
                        } catch (t: Throwable) {
                            Log.e(TAG, "Failed to export recording", t)
                            success = false
                        } finally {
                            engine.release()
                        }
                    }
                } finally {
                    _isProcessing.value = false
                    _processingMessage.value = R.string.preprocessing_message
                    if (!success) {
                        _processingProgress.value = 0
                    }
                }

                if (success) {
                    recording.cacheKey?.let { rememberCachedSource(it) }
                    val messageArg = when (val result = outcome) {
                        is ExportOutcome.Local -> result.file.name
                        is ExportOutcome.Document -> result.document.name ?: result.document.uri.toString()
                        else -> null
                    }
                    _statusMessage.value = PlaybackMessage(R.string.export_success, listOfNotNull(messageArg))
                } else {
                    _statusMessage.value = PlaybackMessage(R.string.export_failure)
                }
            }
        }
    }

    private fun startPositionUpdates() {
        stopPositionUpdates()

        positionUpdateJob = viewModelScope.launch {
            while (isActive) {
                val positionSeconds = playbackEngine?.getPosition() ?: 0.0
                _currentPosition.value = (positionSeconds * 1000).toLong()

                val state = playbackEngine?.getState()
                if (state == NativePlaybackEngine.State.STOPPED) {
                    _isPlaying.value = false
                    break
                }

                delay(100)
            }
        }
    }

    private fun stopPositionUpdates() {
        positionUpdateJob?.cancel()
        positionUpdateJob = null
    }

    fun selectIRPreset(preset: IRPreset) {
        _selectedIRPreset.value = preset
    }

    fun setPlaying(playing: Boolean) {
        _isPlaying.value = playing
    }

    fun updatePosition(positionMs: Long) {
        _currentPosition.value = positionMs
    }

    fun clearStatusMessage() {
        _statusMessage.value = null
    }

    fun deleteRecording(recording: Recording) {
        viewModelScope.launch(Dispatchers.IO) {
            val deleted = runCatching {
                when {
                    recording.file?.exists() == true -> recording.file.delete()
                    recording.documentUri != null -> {
                        val context = appContext ?: return@runCatching false
                        DocumentFile.fromSingleUri(context, recording.documentUri)?.delete() ?: false
                    }
                    else -> false
                }
            }.getOrElse { throwable ->
                Log.e(TAG, "Failed to delete recording", throwable)
                false
            }

            if (deleted) {
                withContext(Dispatchers.Main) {
                    if (_selectedRecording.value?.uniqueId == recording.uniqueId) {
                        clearSelection()
                    }
                }
                scanRecordings()
            } else {
                _statusMessage.postValue(PlaybackMessage(R.string.delete_failed))
            }
        }
    }

    private fun tryReuseCachedPreRender(recording: Recording): Boolean {
        val engine = playbackEngine ?: return false
        val cacheDir = cacheDirectory ?: return false
        val cacheKey = recording.cacheKey ?: return false
        val lastSource = lastCachedSource() ?: return false

        if (lastSource != cacheKey) {
            return false
        }

        val cacheFile = File(cacheDir, CACHE_FILE_NAME)
        if (!cacheFile.exists()) {
            clearCachedSource()
            return false
        }

        val reused = runCatching {
            engine.useCachedPreRender(cacheKey)
        }.onFailure { throwable ->
            Log.e(TAG, "Failed to reuse cached pre-render", throwable)
        }.getOrDefault(false)

        if (reused) {
            rememberCachedSource(cacheKey)
        } else {
            clearCachedSource()
        }

        return reused
    }

    private fun rememberCachedSource(sourceKey: String) {
        preferences?.edit()?.putString(PREF_LAST_CACHE_SOURCE, sourceKey)?.apply()
    }

    private fun clearCachedSource() {
        preferences?.edit()?.remove(PREF_LAST_CACHE_SOURCE)?.apply()
    }

    private fun lastCachedSource(): String? = preferences?.getString(PREF_LAST_CACHE_SOURCE, null)

    fun getTotalStorageBytes(): Long {
        return _recordings.value?.sumOf { it.fileSizeBytes } ?: 0L
    }

    fun getFormattedStorageSize(): String {
        val bytes = getTotalStorageBytes()
        val mb = bytes / (1024f * 1024f)
        return when {
            mb >= 1024 -> String.format("%.1f GB", mb / 1024f)
            mb >= 1 -> String.format("%.0f MB", mb)
            else -> String.format("%.0f KB", bytes / 1024f)
        }
    }

    override fun onCleared() {
        super.onCleared()
        stopPlayback()
        playbackEngine?.release()
        playbackEngine = null
    }

    private suspend fun ensurePreprocessed(): Boolean {
        val engine = playbackEngine ?: return false
        if (engine.isPreRenderReady()) {
            _processingProgress.value = 100
            return true
        }

        return preprocessMutex.withLock {
            if (engine.isPreRenderReady()) {
                _processingProgress.value = 100
                return@withLock true
            }

            try {
                _processingMessage.value = R.string.preprocessing_message
                _isProcessing.value = true
                _processingProgress.value = 0

                val success = coroutineScope {
                    val prepareDeferred = async(Dispatchers.IO) { engine.preparePreRender() }
                    val progressJob = launch {
                        while (isActive) {
                            val progress = engine.getPreRenderProgress().coerceIn(0, 100)
                            _processingProgress.value = progress

                            if (prepareDeferred.isCompleted && progress >= 100) {
                                break
                            }

                            delay(150)
                        }
                    }

                    val result = prepareDeferred.await()
                    progressJob.cancelAndJoin()

                    if (result) {
                        _processingProgress.value = 100
                    } else {
                        _processingProgress.value = 0
                    }

                    result
                }

                if (success) {
                    val newDuration = engine.getDuration()
                    _totalDuration.value = (newDuration * 1000).toLong()
                    _currentPosition.value = 0L
                    selectedRecording.value?.cacheKey?.let { key -> rememberCachedSource(key) }
                } else {
                    _statusMessage.value = PlaybackMessage(R.string.preprocessing_failed)
                    clearCachedSource()
                }

                success
            } finally {
                _isProcessing.value = false
            }
        }
    }

    private fun playbackCacheForRecording(recording: Recording): File? {
        val cacheDir = cacheDirectory ?: return null
        val cacheKey = recording.cacheKey ?: return null
        if (cacheKey != lastCachedSource()) {
            return null
        }

        val cacheFile = File(cacheDir, CACHE_FILE_NAME)
        return cacheFile.takeIf { it.exists() && it.length() > 0 }
    }

    private fun reuseCachedPreRenderForExport(
        cachedFile: File,
        recording: Recording,
        context: Context,
        storageInfo: StorageLocationManager.StorageInfo?
    ): ExportOutcome? {
        return when {
            recording.file?.exists() == true -> {
                val recordingDir = recording.file.parentFile ?: return null
                val exportDir = StorageLocationManager.ensureExportsDirectory(recordingDir)
                val exportFile = File(exportDir, buildExportFileName(recording))
                runCatching {
                    cachedFile.inputStream().use { input ->
                        exportFile.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    ExportOutcome.Local(exportFile)
                }.getOrNull()
            }
            recording.documentUri != null -> {
                val treeUri = storageInfo?.treeUri ?: return null
                val exportsDir = StorageLocationManager.ensureExportsDocumentDirectory(context, treeUri)
                    ?: return null
                val targetName = buildExportFileName(recording)
                val document = StorageLocationManager.createOrReplaceDocumentFile(exportsDir, targetName)
                    ?: return null

                val copied = context.contentResolver.openOutputStream(document.uri, "w")?.use { output ->
                    cachedFile.inputStream().use { input -> input.copyTo(output) }
                    true
                } ?: false

                if (copied) {
                    ExportOutcome.Document(document)
                } else {
                    runCatching { document.delete() }
                    null
                }
            }
            else -> null
        }
    }
}

data class PlaybackMessage(@StringRes val resId: Int, val args: List<Any> = emptyList())

private fun buildExportFileName(recording: Recording, suffix: String = "binaural"): String {
    val originalName = recording.file?.name ?: recording.fileName
    val base = originalName.substringBeforeLast('.', originalName).ifBlank { originalName }
    return "${base}_${suffix}.wav"
}
