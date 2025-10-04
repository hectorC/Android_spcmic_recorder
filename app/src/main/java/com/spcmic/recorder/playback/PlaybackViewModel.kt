package com.spcmic.recorder.playback

import android.os.Environment
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * ViewModel for playback screen
 */
class PlaybackViewModel : ViewModel() {
    
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
        _selectedRecording.value = recording
        _totalDuration.value = recording.durationMs
        _currentPosition.value = 0L
        _isPlaying.value = false
    }
    
    /**
     * Clear selected recording
     */
    fun clearSelection() {
        _selectedRecording.value = null
        _isPlaying.value = false
        _currentPosition.value = 0L
        _totalDuration.value = 0L
    }
    
    /**
     * Select IR preset
     */
    fun selectIRPreset(preset: IRPreset) {
        _selectedIRPreset.value = preset
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
}
