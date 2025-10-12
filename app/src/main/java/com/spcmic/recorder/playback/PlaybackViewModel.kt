package com.spcmic.recorder.playback

import android.content.SharedPreferences
import android.util.Log
import android.content.res.AssetManager
import com.spcmic.recorder.R
import com.spcmic.recorder.StorageLocationManager
import androidx.annotation.StringRes
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import java.io.File

/**
 * ViewModel for playback screen
 */
class PlaybackViewModel : ViewModel() {
    companion object {
    private const val TAG = "PlaybackViewModel"
    const val PREFS_NAME = "playback_cache_prefs"
        private const val PREF_LAST_CACHE_SOURCE = "last_cached_source"
        private const val CACHE_FILE_NAME = "playback_cache.wav"
    }
    
    private val _recordings = MutableLiveData<List<Recording>>(emptyList())
    val recordings: LiveData<List<Recording>> = _recordings
    
    private val _isLoading = MutableLiveData<Boolean>(false)
    val isLoading: LiveData<Boolean> = _isLoading
    
    private val _selectedRecording = MutableLiveData<Recording?>()
    val selectedRecording: LiveData<Recording?> = _selectedRecording
    
    private val _selectedIRPreset = MutableLiveData<IRPreset>(IRPreset.BINAURAL)
    val selectedIRPreset: LiveData<IRPreset> = _selectedIRPreset
    
    private val _isPlaying = MutableLiveData<Boolean>(false)
    val isPlaying: LiveData<Boolean> = _isPlaying
    
    private val _currentPosition = MutableLiveData<Long>(0L)
    val currentPosition: LiveData<Long> = _currentPosition
    
    private val _totalDuration = MutableLiveData<Long>(0L)
    val totalDuration: LiveData<Long> = _totalDuration

    private val _isProcessing = MutableLiveData(false)
    val isProcessing: LiveData<Boolean> = _isProcessing

    private val _processingProgress = MutableLiveData(0)
    val processingProgress: LiveData<Int> = _processingProgress

    private val _processingMessage = MutableLiveData(R.string.preprocessing_message)
    val processingMessage: LiveData<Int> = _processingMessage

    private val _statusMessage = MutableLiveData<PlaybackMessage?>()
    val statusMessage: LiveData<PlaybackMessage?> = _statusMessage
    
    // Native playback engine
    private var playbackEngine: NativePlaybackEngine? = null
    private var positionUpdateJob: Job? = null
    private var assetManager: AssetManager? = null
    private var cacheDirectory: String? = null
    private var preferences: SharedPreferences? = null
    private val preprocessMutex = Mutex()

    private val _playbackGainDb = MutableLiveData(0f)
    val playbackGainDb: LiveData<Float> = _playbackGainDb

    private val _isLooping = MutableLiveData(false)
    val isLooping: LiveData<Boolean> = _isLooping

    private val _storagePath = MutableLiveData<String>()
    val storagePath: LiveData<String> = _storagePath

    private var recordingsDirectory: File? = null

    fun setRecordingsDirectory(directory: File) {
        recordingsDirectory = directory
        _storagePath.value = directory.absolutePath
    }
    
    /**
     * Scan directory for recordings
     */
    fun scanRecordings() {
        viewModelScope.launch {
            _isLoading.value = true
            val targetDir = recordingsDirectory
            if (targetDir == null) {
                _recordings.value = emptyList()
                _isLoading.value = false
                return@launch
            }
            
            val recordingList = withContext(Dispatchers.IO) {
                if (!targetDir.exists()) {
                    targetDir.mkdirs()
                }

                targetDir.listFiles()?.asSequence()
                    ?.filter { file -> file.isFile && file.parentFile == targetDir }
                    ?.filter { file -> file.name.endsWith(".wav", ignoreCase = true) }
                    ?.mapNotNull { file -> WavMetadataParser.createRecording(file) }
                    ?.sortedByDescending { it.dateTime }
                    ?.toList()
                    ?: emptyList()
            }
            
            _recordings.value = recordingList
            _isLoading.value = false
        }
    }
    
    /**
     * Select a recording for playback
     */
    fun selectRecording(recording: Recording) {
        // Stop current playback if any
        stopPlayback()
        
        // Load new file
        viewModelScope.launch(Dispatchers.IO) {
            try {
                if (playbackEngine == null) {
                    playbackEngine = NativePlaybackEngine()
                }
                assetManager?.let { playbackEngine?.setAssetManager(it) }
                cacheDirectory?.let { playbackEngine?.setCacheDirectory(it) }
                playbackEngine?.setPlaybackGain(_playbackGainDb.value ?: 0f)
                playbackEngine?.setLooping(_isLooping.value ?: false)

                if (playbackEngine?.loadFile(recording.file.absolutePath) == true) {
                    val reused = tryReuseCachedPreRender(recording)
                    val durationSeconds = playbackEngine?.getDuration() ?: 0.0
                    
                    withContext(Dispatchers.Main) {
                        _selectedRecording.value = recording
                        _totalDuration.value = (durationSeconds * 1000).toLong()
                        _currentPosition.value = 0L
                        _isPlaying.value = false
                        _isProcessing.value = false
                        _processingProgress.value = if (reused) 100 else 0
                        _processingMessage.value = R.string.preprocessing_message
                        if (reused) {
                            _statusMessage.value = PlaybackMessage(R.string.playback_cached_ready)
                        }
                    }
                } else {
                    withContext(Dispatchers.Main) {
                        // TODO: Show error to user
                        android.util.Log.e("PlaybackViewModel", "Failed to load file: ${recording.file.absolutePath}")
                    }
                }
            } catch (e: Exception) {
                android.util.Log.e("PlaybackViewModel", "Error loading file", e)
            }
        }
    }
    
    /**
     * Clear selected recording
     */
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
    
    /**
     * Start playback
     */
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
    
    /**
     * Pause playback
     */
    fun pause() {
        playbackEngine?.pause()
        _isPlaying.value = false
        stopPositionUpdates()
    }
    
    /**
     * Stop playback
     */
    fun stopPlayback() {
        playbackEngine?.stop()
        _isPlaying.value = false
        _currentPosition.value = 0L
        stopPositionUpdates()
    }
    
    /**
     * Seek to position
     */
    fun seekTo(positionMs: Long) {
        val positionSeconds = positionMs / 1000.0
        playbackEngine?.seek(positionSeconds)
        _currentPosition.value = positionMs
    }

    fun exportRecording(recording: Recording) {
        viewModelScope.launch {
            val assets = assetManager
            val cacheDir = cacheDirectory
            if (assets == null || cacheDir == null) {
                _statusMessage.value = PlaybackMessage(R.string.export_failure)
                return@launch
            }

            preprocessMutex.withLock {
                _processingMessage.value = R.string.exporting_message
                _processingProgress.value = 0
                _isProcessing.value = true

                var success = false
                var exportedFile: File? = null

                try {
                    val engine = NativePlaybackEngine()
                    engine.setAssetManager(assets)
                    engine.setCacheDirectory(cacheDir)

                    try {
                        val loadSuccess = withContext(Dispatchers.IO) {
                            engine.loadFile(recording.file.absolutePath)
                        }

                        if (loadSuccess) {
                            success = coroutineScope {
                                val prepareDeferred = async(Dispatchers.IO) {
                                    engine.preparePreRender()
                                }

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

                                exportedFile = withContext(Dispatchers.IO) {
                                    val recordingDir = recording.file.parentFile ?: return@withContext null
                                    val exportDir = StorageLocationManager.ensureExportsDirectory(recordingDir)

                                    val baseName = recording.file.nameWithoutExtension
                                    val exportFile = File(exportDir, "${baseName}_binaural.wav")
                                    if (engine.exportPreRendered(exportFile.absolutePath)) exportFile else null
                                }

                                exportedFile != null
                            }
                        }
                    } finally {
                        engine.release()
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "Failed to export recording", e)
                    success = false
                } finally {
                    _processingProgress.value = if (success) 100 else 0
                    _isProcessing.value = false
                    _processingMessage.value = R.string.preprocessing_message
                }

                if (success) {
                    rememberCachedSource(recording.file.absolutePath)
                    val messageArg = exportedFile?.let { "Exports/${it.name}" }
                    _statusMessage.value = PlaybackMessage(
                        R.string.export_success,
                        listOfNotNull(messageArg ?: exportedFile?.name)
                    )
                } else {
                    _statusMessage.value = PlaybackMessage(R.string.export_failure)
                }
            }
        }
    }
    
    /**
     * Start periodic position updates
     */
    private fun startPositionUpdates() {
        stopPositionUpdates()
        
        positionUpdateJob = viewModelScope.launch {
            while (isActive) {
                val positionSeconds = playbackEngine?.getPosition() ?: 0.0
                _currentPosition.value = (positionSeconds * 1000).toLong()
                
                // Check if playback finished
                val state = playbackEngine?.getState()
                if (state == NativePlaybackEngine.State.STOPPED) {
                    _isPlaying.value = false
                    break
                }
                
                delay(100)  // Update every 100ms
            }
        }
    }
    
    /**
     * Stop position updates
     */
    private fun stopPositionUpdates() {
        positionUpdateJob?.cancel()
        positionUpdateJob = null
    }
    
    /**
     * Select IR preset
     */
    fun selectIRPreset(preset: IRPreset) {
        _selectedIRPreset.value = preset
    }

    fun setAssetManager(manager: AssetManager) {
        assetManager = manager
        playbackEngine?.setAssetManager(manager)
    }

    fun setPreferences(prefs: SharedPreferences) {
        preferences = prefs
    }

    fun setCacheDirectory(path: String) {
        cacheDirectory = path
        playbackEngine?.setCacheDirectory(path)
    }
    
    /**
     * Update playback state
     */
    fun setPlaying(playing: Boolean) {
        _isPlaying.value = playing
    }
    
    /**
     * Update current position
     */
    fun updatePosition(positionMs: Long) {
        _currentPosition.value = positionMs
    }

    fun clearStatusMessage() {
    _statusMessage.value = null
    }

    fun deleteRecording(recording: Recording) {
        viewModelScope.launch(Dispatchers.IO) {
            val deleted = runCatching {
                if (recording.file.exists()) {
                    recording.file.delete()
                } else {
                    false
                }
            }.getOrElse { throwable ->
                Log.e(TAG, "Failed to delete recording", throwable)
                false
            }

            if (deleted) {
                if (_selectedRecording.value?.file == recording.file) {
                    withContext(Dispatchers.Main) {
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
        val lastSource = lastCachedSource() ?: return false

        if (lastSource != recording.file.absolutePath) {
            return false
        }

        val cacheFile = File(cacheDir, CACHE_FILE_NAME)
        if (!cacheFile.exists()) {
            clearCachedSource()
            return false
        }

        val reused = runCatching {
            engine.useCachedPreRender(recording.file.absolutePath)
        }.onFailure { throwable ->
            Log.e(TAG, "Failed to reuse cached pre-render", throwable)
        }.getOrDefault(false)

        if (reused) {
            rememberCachedSource(recording.file.absolutePath)
        } else {
            clearCachedSource()
        }

        return reused
    }

    private fun rememberCachedSource(path: String) {
        preferences?.edit()?.putString(PREF_LAST_CACHE_SOURCE, path)?.apply()
    }

    private fun clearCachedSource() {
        preferences?.edit()?.remove(PREF_LAST_CACHE_SOURCE)?.apply()
    }

    private fun lastCachedSource(): String? = preferences?.getString(PREF_LAST_CACHE_SOURCE, null)
    
    /**
     * Get total storage used by recordings
     */
    fun getTotalStorageBytes(): Long {
        return _recordings.value?.sumOf { it.fileSizeBytes } ?: 0L
    }
    
    /**
     * Format storage size
     */
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
                    val prepareDeferred = async(Dispatchers.IO) {
                        engine.preparePreRender()
                    }

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
                    selectedRecording.value?.file?.absolutePath?.let { path ->
                        rememberCachedSource(path)
                    }
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
}

data class PlaybackMessage(@StringRes val resId: Int, val args: List<Any> = emptyList())
