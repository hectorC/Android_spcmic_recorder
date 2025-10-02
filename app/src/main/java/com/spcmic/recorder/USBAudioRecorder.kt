package com.spcmic.recorder

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.Environment
import kotlinx.coroutines.*
import java.io.*
import java.text.SimpleDateFormat
import java.util.*

class USBAudioRecorder(
    private val context: Context,
    private val viewModel: MainViewModel
) {
    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
    private var usbDevice: UsbDevice? = null
    private var usbConnection: UsbDeviceConnection? = null
    private var isRecording = false
    private var recordingJob: Job? = null
    private var levelUpdateJob: Job? = null
    private var targetSampleRate = DEFAULT_SAMPLE_RATE
    private var isNativeInitialized = false
    
    private val ACTION_USB_PERMISSION = "com.spcmic.recorder.USB_PERMISSION"
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val action = intent.action
            if (ACTION_USB_PERMISSION == action) {
                synchronized(this) {
                    val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        device?.let {
                            android.util.Log.i("USBAudioRecorder", "USB permission granted for device: ${it.deviceName}")
                            connectToDeviceInternal(it)
                        }
                    } else {
                        android.util.Log.e("USBAudioRecorder", "USB permission denied for device: ${device?.deviceName}")
                    }
                }
            }
        }
    }
    
    // Audio configuration defaults for 84 channels at 24-bit/48kHz
    private val channelCount = 84
    private val bitsPerSample = 24

    init {
        viewModel.selectedSampleRate.value?.let {
            targetSampleRate = it
        }
    }

    // Native library interface
    external fun stringFromJNI(): String
    external fun initializeNativeAudio(deviceFd: Int, sampleRate: Int, channelCount: Int): Boolean
    external fun startRecordingNative(outputPath: String): Boolean
    external fun stopRecordingNative(): Boolean
    external fun startMonitoringNative(): Boolean
    external fun stopMonitoringNative()
    external fun getChannelLevelsNative(): FloatArray?
    external fun releaseNativeAudio()
    external fun getSupportedSampleRatesNative(): IntArray?
    external fun supportsContinuousSampleRateNative(): Boolean
    external fun getContinuousSampleRateRangeNative(): IntArray?
    external fun getEffectiveSampleRateNative(): Int
    external fun setTargetSampleRateNative(sampleRate: Int): Boolean
    
    companion object {
        private const val DEFAULT_SAMPLE_RATE = 48000
        init {
            try {
                System.loadLibrary("spcmic_recorder")
                android.util.Log.i("USBAudioRecorder", "Native library loaded successfully")
            } catch (e: UnsatisfiedLinkError) {
                android.util.Log.e("USBAudioRecorder", "Failed to load native library: ${e.message}")
                // Continue without native functionality for now
            }
        }
    }
    
    fun connectToDevice(device: UsbDevice): Boolean {
        android.util.Log.i("USBAudioRecorder", "Attempting to connect to USB device: ${device.deviceName}")
        
        // Check if permission is already granted
        if (usbManager.hasPermission(device)) {
            android.util.Log.i("USBAudioRecorder", "USB permission already granted")
            return connectToDeviceInternal(device)
        } else {
            android.util.Log.i("USBAudioRecorder", "Requesting USB permission")
            requestUSBPermission(device)
            return true // Return true as permission request is async
        }
    }
    
    private fun requestUSBPermission(device: UsbDevice) {
        val permissionIntent = PendingIntent.getBroadcast(
            context, 0, Intent(ACTION_USB_PERMISSION), 
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        context.registerReceiver(usbReceiver, filter, Context.RECEIVER_EXPORTED)
        
        usbManager.requestPermission(device, permissionIntent)
    }
    
    private fun connectToDeviceInternal(device: UsbDevice): Boolean {
        usbDevice = device
        
        android.util.Log.i("USBAudioRecorder", "Opening USB device connection")
        usbConnection = usbManager.openDevice(device)
        if (usbConnection == null) {
            android.util.Log.e("USBAudioRecorder", "Failed to open USB device connection")
            return false
        }
        
        // CRITICAL: Claim Interface 3 specifically for SPCMic 84-channel audio
        android.util.Log.i("USBAudioRecorder", "Claiming SPCMic Interface 3 for 84-channel audio streaming")
        
        var interface3Claimed = false
        try {
            // Find and claim Interface 3 specifically (SPCMic audio interface)
            for (i in 0 until device.configurationCount) {
                val config = device.getConfiguration(i)
                for (j in 0 until config.interfaceCount) {
                    val usbInterface = config.getInterface(j)
                    if (usbInterface.id == 3 && usbInterface.interfaceClass == 1) { // Interface 3, Audio class
                        val claimed = usbConnection!!.claimInterface(usbInterface, true) // Force claim
                        android.util.Log.i("USBAudioRecorder", "Claimed SPCMic Interface 3: $claimed")
                        interface3Claimed = claimed
                        break
                    }
                }
                if (interface3Claimed) break
            }
            
            if (!interface3Claimed) {
                android.util.Log.w("USBAudioRecorder", "Could not find or claim Interface 3 - trying all audio interfaces")
                // Fallback: claim all audio interfaces if Interface 3 not found
                for (i in 0 until device.configurationCount) {
                    val config = device.getConfiguration(i)
                    for (j in 0 until config.interfaceCount) {
                        val usbInterface = config.getInterface(j)
                        if (usbInterface.interfaceClass == 1) { // Audio class
                            val claimed = usbConnection!!.claimInterface(usbInterface, true)
                            android.util.Log.i("USBAudioRecorder", "Claimed audio interface ${usbInterface.id}: $claimed")
                        }
                    }
                }
            }
        } catch (e: Exception) {
            android.util.Log.e("USBAudioRecorder", "Exception while claiming interfaces", e)
        }
        
        // Small delay to ensure interface claiming is complete
        Thread.sleep(100)
        
        // Get file descriptor for native USB communication
        val deviceFd = usbConnection!!.fileDescriptor
        android.util.Log.i("USBAudioRecorder", "USB device FD: $deviceFd")

        targetSampleRate = viewModel.selectedSampleRate.value ?: targetSampleRate

        // Initialize native audio interface
        val success = initializeNativeAudio(deviceFd, targetSampleRate, channelCount)
        if (success) {
            isNativeInitialized = true
            android.util.Log.i("USBAudioRecorder", "Native audio initialized: ${stringFromJNI()}")
            refreshSampleRateCapabilities(targetSampleRate)
            // TEMPORARILY DISABLED: Level monitoring disabled to focus on clean recording
            // startLevelMonitoring()
        } else {
            isNativeInitialized = false
            android.util.Log.e("USBAudioRecorder", "Failed to initialize native audio")
            usbConnection?.close()
            usbConnection = null
        }

        return success
    }
    
    fun startRecording(): Boolean {
        android.util.Log.i("USBAudioRecorder", "startRecording called - isRecording: $isRecording, usbDevice: $usbDevice, usbConnection: $usbConnection")
        
        if (isRecording) {
            android.util.Log.w("USBAudioRecorder", "Already recording")
            return false
        }
        
        if (usbDevice == null) {
            android.util.Log.e("USBAudioRecorder", "No USB device connected")
            return false
        }
        
        if (usbConnection == null) {
            android.util.Log.e("USBAudioRecorder", "No USB connection established")
            return false
        }
        
        try {
            val dateFormat = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
            val timestamp = dateFormat.format(Date())
            val fileName = "spcmic_recording_$timestamp.wav"
            
            val documentsDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOCUMENTS)
            val appDir = File(documentsDir, "SPCMicRecorder")
            appDir.mkdirs()
            val outputFile = File(appDir, fileName)
            
            android.util.Log.i("USBAudioRecorder", "Starting recording to: ${outputFile.absolutePath}")
            
            // Start native recording
            val success = startRecordingNative(outputFile.absolutePath)
            if (success) {
                isRecording = true
                android.util.Log.i("USBAudioRecorder", "Started recording to: ${outputFile.absolutePath}")
                
                // Start recording monitoring job
                recordingJob = CoroutineScope(Dispatchers.IO).launch {
                    monitorRecording()
                }
            } else {
                android.util.Log.e("USBAudioRecorder", "Failed to start native recording")
            }
            
            return success
            
        } catch (e: Exception) {
            android.util.Log.e("USBAudioRecorder", "Exception in startRecording", e)
            return false
        }
    }
    
    private suspend fun monitorRecording() {
        while (isRecording) {
            // Recording monitoring can be added here if needed
            // The native code handles the actual recording
            delay(100) // Check every 100ms
        }
    }

    private fun refreshSampleRateCapabilities(requestedSampleRate: Int) {
        if (!isNativeInitialized) {
            viewModel.updateSampleRateOptions(emptyList(), false, null, requestedSampleRate, requestedSampleRate)
            return
        }

        val discreteRates = getSupportedSampleRatesNative()
            ?.filter { it > 0 }
            ?.distinct()
            ?.sorted()
            ?: emptyList()

        val continuousSupported = supportsContinuousSampleRateNative()
        val continuousArray = if (continuousSupported) getContinuousSampleRateRangeNative() else null
        val continuousRange = continuousArray
            ?.takeIf { it.size >= 2 }
            ?.let { Pair(it[0], it[1]) }

        val negotiated = getEffectiveSampleRateNative().takeIf { it > 0 } ?: requestedSampleRate

        val aggregated = linkedSetOf<Int>()
        aggregated.addAll(discreteRates)
        aggregated.add(negotiated)
        if (!continuousSupported || discreteRates.isEmpty()) {
            aggregated.add(requestedSampleRate)
        }
        continuousRange?.let {
            aggregated.add(it.first)
            aggregated.add(it.second)
        }

        val ratesForUi = aggregated.filter { it > 0 }.sorted()

        viewModel.updateSampleRateOptions(ratesForUi, continuousSupported, continuousRange, requestedSampleRate, negotiated)

        val desiredSelection = viewModel.selectedSampleRate.value ?: requestedSampleRate
        targetSampleRate = desiredSelection

        if (desiredSelection != negotiated && desiredSelection != requestedSampleRate) {
            val applied = setTargetSampleRateNative(desiredSelection)
            if (applied) {
                val refreshedNegotiated = getEffectiveSampleRateNative().takeIf { it > 0 } ?: desiredSelection
                viewModel.setNegotiatedSampleRate(refreshedNegotiated)
            } else {
                android.util.Log.w("USBAudioRecorder", "Device rejected sample rate ${desiredSelection} Hz; reverting to ${negotiated} Hz")
                viewModel.setSelectedSampleRate(negotiated)
                viewModel.setNegotiatedSampleRate(negotiated)
                targetSampleRate = negotiated
            }
        } else {
            viewModel.setNegotiatedSampleRate(negotiated)
        }
    }
    
    private fun startLevelMonitoring() {
        // Start native monitoring to continuously read audio data for level meters
        val monitoringStarted = startMonitoringNative()
        android.util.Log.i("USBAudioRecorder", "Native monitoring started: $monitoringStarted")
        
        levelUpdateJob = CoroutineScope(Dispatchers.IO).launch {
            while (usbConnection != null) {
                val levels = getChannelLevelsNative()
                if (levels != null && levels.size == channelCount) {
                    withContext(Dispatchers.Main) {
                        viewModel.updateChannelLevels(levels)
                    }
                }
                delay(50) // Update levels every 50ms (20 FPS)
            }
        }
    }
    
    fun stopRecording() {
        if (!isRecording) return
        
        android.util.Log.i("USBAudioRecorder", "Stopping recording")
        
        isRecording = false
        recordingJob?.cancel()
        
        // Stop native recording
        stopRecordingNative()
        
        android.util.Log.i("USBAudioRecorder", "Recording stopped")
    }

    fun onSampleRateSelected(rate: Int): Boolean {
        targetSampleRate = rate

        if (!isNativeInitialized) {
            viewModel.setSelectedSampleRate(rate)
            return false
        }

        val applied = setTargetSampleRateNative(rate)
        if (applied) {
            viewModel.setSelectedSampleRate(rate)
            val negotiated = getEffectiveSampleRateNative().takeIf { it > 0 } ?: rate
            viewModel.setNegotiatedSampleRate(negotiated)
            refreshSampleRateCapabilities(rate)
            return true
        }

        android.util.Log.w("USBAudioRecorder", "Device rejected requested sample rate $rate Hz")
        return false
    }
    
    fun release() {
        android.util.Log.i("USBAudioRecorder", "Releasing USB audio recorder")
        
        stopRecording()
        levelUpdateJob?.cancel()
        
        // Stop native monitoring
        stopMonitoringNative()
        
        // Release native resources
        releaseNativeAudio()
        isNativeInitialized = false
        
        // Unregister USB receiver
        try {
            context.unregisterReceiver(usbReceiver)
        } catch (e: IllegalArgumentException) {
            // Receiver was not registered
        }
        
        usbConnection?.close()
        usbConnection = null
        usbDevice = null
        
        android.util.Log.i("USBAudioRecorder", "USB audio recorder released")
    }
}