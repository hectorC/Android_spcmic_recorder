package com.spcmic.recorder

import android.hardware.usb.UsbDevice
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

class MainViewModel : ViewModel() {

    companion object {
        private const val DEFAULT_SAMPLE_RATE = 48000
    }

    private val _isRecording = MutableLiveData(false)
    val isRecording: LiveData<Boolean> = _isRecording
    
    private val _isUSBDeviceConnected = MutableLiveData(false)
    val isUSBDeviceConnected: LiveData<Boolean> = _isUSBDeviceConnected
    
    private val _recordingTime = MutableLiveData(0L)
    val recordingTime: LiveData<Long> = _recordingTime
    
    private val _supportedSampleRates = MutableLiveData<List<Int>>(emptyList())
    val supportedSampleRates: LiveData<List<Int>> = _supportedSampleRates

    private val _selectedSampleRate = MutableLiveData(DEFAULT_SAMPLE_RATE)
    val selectedSampleRate: LiveData<Int> = _selectedSampleRate

    private val _negotiatedSampleRate = MutableLiveData(DEFAULT_SAMPLE_RATE)
    val negotiatedSampleRate: LiveData<Int> = _negotiatedSampleRate

    private val _supportsContinuousSampleRate = MutableLiveData(false)
    val supportsContinuousSampleRate: LiveData<Boolean> = _supportsContinuousSampleRate

    private val _continuousSampleRateRange = MutableLiveData<Pair<Int, Int>?>(null)
    val continuousSampleRateRange: LiveData<Pair<Int, Int>?> = _continuousSampleRateRange

    private val _isClipping = MutableLiveData(false)
    val isClipping: LiveData<Boolean> = _isClipping

    private val _recordingFileName = MutableLiveData<String?>(null)
    val recordingFileName: LiveData<String?> = _recordingFileName

    private val _storagePath = MutableLiveData<String>()
    val storagePath: LiveData<String> = _storagePath
    
    private var recordingJob: Job? = null
    private var currentUSBDevice: UsbDevice? = null
    
    init {
        resetSampleRateState()
    _isClipping.value = false
    }
    
    fun setUSBDevice(device: UsbDevice?) {
        currentUSBDevice = device
        _isUSBDeviceConnected.value = device != null
        if (device == null) {
            resetSampleRateState()
            _isClipping.value = false
            _recordingFileName.value = null
        }
    }
    
    fun startRecording() {
        if (_isRecording.value == true) return
        
        _isRecording.value = true
        _recordingTime.value = 0L
        
        recordingJob = viewModelScope.launch {
            var seconds = 0L
            while (_isRecording.value == true) {
                delay(1000)
                seconds++
                _recordingTime.value = seconds
            }
        }
    }
    
    fun stopRecording() {
        _isRecording.value = false
        recordingJob?.cancel()
        recordingJob = null
    }
    
    fun updateSampleRateOptions(
        rates: List<Int>,
        supportsContinuous: Boolean,
        continuousRange: Pair<Int, Int>?,
        requestedRate: Int,
        negotiatedRate: Int
    ) {
        val sanitizedRates = rates.distinct().sorted()
        _supportedSampleRates.value = sanitizedRates
        _supportsContinuousSampleRate.value = supportsContinuous
        _continuousSampleRateRange.value = if (supportsContinuous) continuousRange else null

        val desiredSelection = when {
            sanitizedRates.contains(requestedRate) -> requestedRate
            sanitizedRates.isNotEmpty() -> sanitizedRates.first()
            else -> DEFAULT_SAMPLE_RATE
        }

        _selectedSampleRate.value = desiredSelection
        _negotiatedSampleRate.value = negotiatedRate
    }

    fun setSelectedSampleRate(rate: Int) {
        _selectedSampleRate.value = rate
    }

    fun setNegotiatedSampleRate(rate: Int) {
        _negotiatedSampleRate.value = rate
    }

    fun setClipping(isClipping: Boolean) {
        _isClipping.value = isClipping
    }

    fun clearClipping() {
        _isClipping.value = false
    }

    fun setRecordingFileName(name: String?) {
        _recordingFileName.value = name
    }

    fun setStoragePath(path: String) {
        _storagePath.value = path
    }

    private fun resetSampleRateState() {
        _supportedSampleRates.value = emptyList()
        _supportsContinuousSampleRate.value = false
        _continuousSampleRateRange.value = null
        _selectedSampleRate.value = DEFAULT_SAMPLE_RATE
        _negotiatedSampleRate.value = DEFAULT_SAMPLE_RATE
    }
    
    override fun onCleared() {
        super.onCleared()
        recordingJob?.cancel()
    }
}