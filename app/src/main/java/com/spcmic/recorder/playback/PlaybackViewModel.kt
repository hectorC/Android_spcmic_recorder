package com.spcmic.recorder.playback

import android.content.SharedPreferences
import android.os.Environment
import android.util.Log
import android.content.res.AssetManager
import com.spcmic.recorder.R
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

    private val _isPreprocessing = MutableLiveData(false)
    val isPreprocessing: LiveData<Boolean> = _isPreprocessing

    private val _preprocessProgress = MutableLiveData(0)
    val preprocessProgress: LiveData<Int> = _preprocessProgress

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
    
    /**
     * Scan directory for recordings
     */
    fun scanRecordings() {
        viewModelScope.launch {
            _isLoading.value = true
            
            val recordingList = withContext(Dispatchers.IO) {
                val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
                val appDir = File(documentsDir, "SPCMicRecorder")
                
                if (!appDir.exists()) {
                    return@withContext emptyList<Recording>()
                }
                
                appDir.listFiles { file ->
                    file.isFile && file.name.endsWith(".wav", ignoreCase = true)
                }?.mapNotNull { file ->
                    WavMetadataParser.createRecording(file)
                }?.sortedByDescending { it.dateTime } ?: emptyList()
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

                if (playbackEngine?.loadFile(recording.file.absolutePath) == true) {
                    val reused = tryReuseCachedPreRender(recording)
                    val durationSeconds = playbackEngine?.getDuration() ?: 0.0
                    
                    withContext(Dispatchers.Main) {
                        _selectedRecording.value = recording
                        _totalDuration.value = (durationSeconds * 1000).toLong()
                        _currentPosition.value = 0L
                        _isPlaying.value = false
                        _isPreprocessing.value = false
                        _preprocessProgress.value = if (reused) 100 else 0
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
        _isPreprocessing.value = false
        _preprocessProgress.value = 0
    }

    fun setPlaybackGain(gainDb: Float) {
        val clamped = gainDb.coerceIn(0f, 48f)
        _playbackGainDb.value = clamped
        playbackEngine?.setPlaybackGain(clamped)
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

    fun exportSelectedRecording(recording: Recording) {
        val current = _selectedRecording.value
        if (current == null || current.file != recording.file) {
            _statusMessage.value = PlaybackMessage(R.string.export_select_recording)
            return
        }

        val engine = playbackEngine ?: return

        viewModelScope.launch {
            val ready = ensurePreprocessed()
            if (!ready) {
                return@launch
            }

            val destination = withContext(Dispatchers.IO) {
                val parent = recording.file.parentFile ?: return@withContext null
                val baseName = recording.file.nameWithoutExtension
                val exportFile = File(parent, "${baseName}_binaural.wav")
                if (engine.exportPreRendered(exportFile.absolutePath)) exportFile else null
            }

            _statusMessage.value = destination?.let {
                PlaybackMessage(R.string.export_success, listOf(it.name))
            } ?: PlaybackMessage(R.string.export_failure)
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
            _preprocessProgress.value = 100
            return true
        }

        return preprocessMutex.withLock {
            if (engine.isPreRenderReady()) {
                _preprocessProgress.value = 100
                return@withLock true
            }

            try {
                _isPreprocessing.value = true
                _preprocessProgress.value = 0

                val success = coroutineScope {
                    val prepareDeferred = async(Dispatchers.IO) {
                        engine.preparePreRender()
                    }

                    val progressJob = launch {
                        while (isActive) {
                            val progress = engine.getPreRenderProgress().coerceIn(0, 100)
                            _preprocessProgress.value = progress

                            if (prepareDeferred.isCompleted && progress >= 100) {
                                break
                            }

                            delay(150)
                        }
                    }

                    val result = prepareDeferred.await()
                    progressJob.cancelAndJoin()

                    if (result) {
                        _preprocessProgress.value = 100
                    } else {
                        _preprocessProgress.value = 0
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
                _isPreprocessing.value = false
            }
        }
    }
}

data class PlaybackMessage(@StringRes val resId: Int, val args: List<Any> = emptyList())
