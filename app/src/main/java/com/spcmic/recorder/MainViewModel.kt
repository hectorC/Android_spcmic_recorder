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
    
    private val _isRecording = MutableLiveData(false)
    val isRecording: LiveData<Boolean> = _isRecording
    
    private val _isUSBDeviceConnected = MutableLiveData(false)
    val isUSBDeviceConnected: LiveData<Boolean> = _isUSBDeviceConnected
    
    private val _recordingTime = MutableLiveData(0L)
    val recordingTime: LiveData<Long> = _recordingTime
    
    private val _channelLevels = MutableLiveData<FloatArray>()
    val channelLevels: LiveData<FloatArray> = _channelLevels
    
    private var recordingJob: Job? = null
    private var currentUSBDevice: UsbDevice? = null
    
    init {
        // Initialize with 84 channels, all at 0 level
        _channelLevels.value = FloatArray(84) { 0f }
    }
    
    fun setUSBDevice(device: UsbDevice?) {
        currentUSBDevice = device
        _isUSBDeviceConnected.value = device != null
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
    
    fun updateChannelLevels(levels: FloatArray) {
        if (levels.size == 84) {
            _channelLevels.value = levels
        }
    }
    
    override fun onCleared() {
        super.onCleared()
        recordingJob?.cancel()
    }
}