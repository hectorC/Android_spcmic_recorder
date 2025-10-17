package com.spcmic.recorder

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import androidx.documentfile.provider.DocumentFile
import androidx.core.content.IntentCompat
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
    private var activeRecordingPfd: ParcelFileDescriptor? = null
    
    private val ACTION_USB_PERMISSION = "com.spcmic.recorder.USB_PERMISSION"
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            val action = intent.action
            if (ACTION_USB_PERMISSION == action) {
                synchronized(this) {
                    val device: UsbDevice? = IntentCompat.getParcelableExtra(intent, UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
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
    external fun startRecordingNative(outputPath: String, gainDb: Float): Boolean
    external fun startRecordingNativeWithFd(fd: Int, absolutePath: String, gainDb: Float): Boolean
    external fun startRecordingFromMonitoringNative(outputPath: String): Boolean
    external fun startRecordingFromMonitoringNativeWithFd(fd: Int, absolutePath: String): Boolean
    external fun stopRecordingNative(): Boolean
    external fun releaseNativeAudio()
    external fun getSupportedSampleRatesNative(): IntArray?
    external fun supportsContinuousSampleRateNative(): Boolean
    external fun getContinuousSampleRateRangeNative(): IntArray?
    external fun getEffectiveSampleRateNative(): Int
    external fun setTargetSampleRateNative(sampleRate: Int): Boolean
    external fun setInterfaceNative(interfaceNum: Int, altSetting: Int): Boolean
    external fun hasClippedNative(): Boolean
    external fun resetClipIndicatorNative()
    external fun setGainNative(gainDb: Float)
    external fun getPeakLevelNative(): Float
    external fun startMonitoringNative(gainDb: Float): Boolean
    external fun stopMonitoringNative(): Boolean
    external fun isMonitoringNative(): Boolean
    external fun isRecordingNative(): Boolean
    
    companion object {
        private const val DEFAULT_SAMPLE_RATE = 48000
        init {
            try {
            try {
                System.loadLibrary("spcmic_recorder")
                android.util.Log.i("USBAudioRecorder", "Native library loaded: spcmic_recorder")
            } catch (e: UnsatisfiedLinkError) {
                android.util.Log.e("USBAudioRecorder", "Failed to load native library for USB recording", e)
            }
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

        // Initialize native USB connection (does NOT set sample rate - device uses hardware default)
        // We pass a sampleRate parameter but the native code just stores it for later use
        val success = initializeNativeAudio(deviceFd, 48000, channelCount)
        if (success) {
            isNativeInitialized = true
            android.util.Log.i("USBAudioRecorder", "Native audio initialized: ${stringFromJNI()}")
            
            // Small delay to let device stabilize
            Thread.sleep(200)
            
            // Query what sample rate the device is ACTUALLY running at (hardware default)
            val actualDeviceRate = getEffectiveSampleRateNative()
            android.util.Log.i("USBAudioRecorder", "Device initialized at hardware default: $actualDeviceRate Hz")
            
            // Sync UI spinner to match device's actual state
            refreshSampleRateCapabilities(actualDeviceRate, syncUiToDevice = true)
            
            // Set device to default 48 kHz if not already at that rate
            if (actualDeviceRate != 48000) {
                android.util.Log.i("USBAudioRecorder", "Changing device from $actualDeviceRate Hz to default 48 kHz")
                onSampleRateSelected(48000)
            } else {
                android.util.Log.i("USBAudioRecorder", "Device already at desired default 48 kHz")
            }
            
            viewModel.clearClipping()
            resetClipIndicatorNative()
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
            android.util.Log.e("USBAudioRecorder", "No USB connection established - connection may have been lost. Please reconnect the device.")
            return false
        }
        
        // Verify USB connection is still valid
        try {
            val fd = usbConnection?.fileDescriptor
            if (fd == -1) {
                android.util.Log.e("USBAudioRecorder", "USB connection invalid (bad file descriptor). Please reconnect the device.")
                usbConnection = null
                return false
            }
        } catch (e: Exception) {
            android.util.Log.e("USBAudioRecorder", "USB connection validation failed: ${e.message}")
            usbConnection = null
            return false
        }
        
        // Check if we're in monitoring mode
        val inMonitoringMode = isMonitoring()
        android.util.Log.i("USBAudioRecorder", "In monitoring mode: $inMonitoringMode")
        
        // Get current gain setting (only needed if starting from IDLE)
        val currentGain = viewModel.gainDb.value ?: 0f
        
        var pendingTarget: StorageLocationManager.RecordingTarget? = null
        try {
            val dateFormat = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault())
            val timestamp = dateFormat.format(Date())
            val fileName = "spcmic_recording_$timestamp.wav"
            
            val target = StorageLocationManager.prepareRecordingTarget(context, fileName)
            if (target == null) {
                android.util.Log.e("USBAudioRecorder", "Failed to resolve recording destination for $fileName")
                return false
            }
            pendingTarget = target

            val success = when {
                target.outputFile != null -> {
                    android.util.Log.i("USBAudioRecorder", "Starting recording to: ${target.outputFile.absolutePath}")
                    if (inMonitoringMode) {
                        // Transition from monitoring to recording
                        startRecordingFromMonitoringNative(target.outputFile.absolutePath)
                    } else {
                        // Start from IDLE (shouldn't happen with new state machine, but keep for safety)
                        startRecordingNative(target.outputFile.absolutePath, currentGain)
                    }
                }
                target.parcelFileDescriptor != null -> {
                    activeRecordingPfd = target.parcelFileDescriptor
                    android.util.Log.i("USBAudioRecorder", "Starting recording via SAF to: ${target.displayLocation}")
                    if (inMonitoringMode) {
                        // Transition from monitoring to recording
                        startRecordingFromMonitoringNativeWithFd(activeRecordingPfd!!.fd, target.displayLocation)
                    } else {
                        // Start from IDLE (shouldn't happen with new state machine, but keep for safety)
                        startRecordingNativeWithFd(activeRecordingPfd!!.fd, target.displayLocation, currentGain)
                    }
                }
                else -> {
                    android.util.Log.e("USBAudioRecorder", "Recording target missing backing file or descriptor")
                    false
                }
            }
            if (success) {
                isRecording = true
                viewModel.clearClipping()
                resetClipIndicatorNative()
                viewModel.setRecordingFileName(fileName)
                android.util.Log.i("USBAudioRecorder", "Started recording to: ${target.displayLocation}")
                
                // Start recording monitoring job with high priority (only if not already monitoring)
                if (!inMonitoringMode) {
                    recordingJob = CoroutineScope(Dispatchers.IO).launch {
                        // Set thread priority to URGENT_AUDIO for real-time performance
                        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
                        android.util.Log.i("USBAudioRecorder", "Set recording thread priority to URGENT_AUDIO")
                        monitorRecording()
                    }
                }
            } else {
                android.util.Log.e("USBAudioRecorder", "Failed to start native recording")
                activeRecordingPfd?.closeSafely()
                activeRecordingPfd = null
                target.documentUri?.let { uri ->
                    runCatching { DocumentFile.fromSingleUri(context, uri)?.delete() }
                }
            }
            
            return success
            
        } catch (e: Exception) {
            android.util.Log.e("USBAudioRecorder", "Exception in startRecording", e)
            activeRecordingPfd?.closeSafely()
            activeRecordingPfd = null
            pendingTarget?.documentUri?.let { uri ->
                runCatching { DocumentFile.fromSingleUri(context, uri)?.delete() }
            }
            return false
        }
    }
    
    private suspend fun monitorRecording() {
        while (isRecording) {
            val clipped = hasClippedNative()
            if (clipped && viewModel.isClipping.value != true) {
                withContext(Dispatchers.Main) {
                    viewModel.setClipping(true)
                }
            }
            delay(100)
        }
    }

    private fun refreshSampleRateCapabilities(requestedSampleRate: Int, syncUiToDevice: Boolean = false) {
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

        if (syncUiToDevice) {
            // Sync UI spinner to match device's actual sample rate (on initial connection/reconnect)
            android.util.Log.i("USBAudioRecorder", "Device reports sample rate: $negotiated Hz - syncing UI spinner")
            viewModel.setSelectedSampleRate(negotiated)
            targetSampleRate = negotiated
        } else {
            // User-initiated change: respect the requested rate but show what device actually reports
            android.util.Log.i("USBAudioRecorder", "User requested: $requestedSampleRate Hz, Device reports: $negotiated Hz")
            targetSampleRate = requestedSampleRate
        }
        
        viewModel.setNegotiatedSampleRate(negotiated)
    }
    
    fun stopRecording() {
        if (!isRecording) return
        
        android.util.Log.i("USBAudioRecorder", "Stopping recording")
        
        isRecording = false
        recordingJob?.cancel()
        
        // Stop native recording
        stopRecordingNative()
        activeRecordingPfd?.closeSafely()
        activeRecordingPfd = null
        
        android.util.Log.i("USBAudioRecorder", "Recording stopped")
    }

    fun resetClipIndicator() {
        if (isNativeInitialized) {
            resetClipIndicatorNative()
        }
        viewModel.clearClipping()
    }

    fun setGain(gainDb: Float) {
        if (isNativeInitialized) {
            setGainNative(gainDb.coerceIn(0f, 64f))
            android.util.Log.i("USBAudioRecorder", "Gain set to $gainDb dB")
        } else {
            android.util.Log.w("USBAudioRecorder", "Cannot set gain - native audio not initialized")
        }
    }

    fun getPeakLevel(): Float {
        return if (isNativeInitialized) {
            getPeakLevelNative()
        } else {
            0f
        }
    }

    fun startMonitoring(gainDb: Float = 0f): Boolean {
        if (!isNativeInitialized) {
            android.util.Log.e("USBAudioRecorder", "Cannot start monitoring - native audio not initialized")
            return false
        }
        
        android.util.Log.i("USBAudioRecorder", "Starting monitoring with gain $gainDb dB")
        val result = startMonitoringNative(gainDb.coerceIn(0f, 64f))
        
        if (result) {
            android.util.Log.i("USBAudioRecorder", "Monitoring started successfully")
            viewModel.startMonitoring()
        } else {
            android.util.Log.e("USBAudioRecorder", "Failed to start monitoring")
        }
        
        return result
    }

    fun stopMonitoring(): Boolean {
        if (!isNativeInitialized) {
            android.util.Log.w("USBAudioRecorder", "Cannot stop monitoring - native audio not initialized")
            return true
        }
        
        android.util.Log.i("USBAudioRecorder", "Stopping monitoring")
        val result = stopMonitoringNative()
        
        if (result) {
            android.util.Log.i("USBAudioRecorder", "Monitoring stopped successfully")
            // Note: ViewModel state is managed by the UI layer (RecordFragment)
            // No need to call viewModel here
        } else {
            android.util.Log.e("USBAudioRecorder", "Failed to stop monitoring")
        }
        
        return result
    }

    fun isMonitoring(): Boolean {
        return if (isNativeInitialized) {
            isMonitoringNative()
        } else {
            false
        }
    }

    fun isRecording(): Boolean {
        return if (isNativeInitialized) {
            isRecordingNative()
        } else {
            false
        }
    }

    fun onSampleRateSelected(rate: Int): Boolean {
        android.util.Log.i("USBAudioRecorder", "=== onSampleRateSelected($rate Hz) START ===")
        targetSampleRate = rate

        if (!isNativeInitialized) {
            android.util.Log.w("USBAudioRecorder", "Native audio not initialized, just updating UI")
            viewModel.setSelectedSampleRate(rate)
            return false
        }

        // Log current state before any changes
        val rateBefore = getEffectiveSampleRateNative()
        android.util.Log.i("USBAudioRecorder", "Current device sample rate BEFORE change: $rateBefore Hz")
        android.util.Log.i("USBAudioRecorder", "Requested sample rate: $rate Hz")

        // Follow Linux USB audio driver pattern:
        // 1. Set interface to alt 0 (disable streaming)
        // 2. Configure sample rate
        // 3. Set interface back to alt 1 (enable streaming)
        
        android.util.Log.i("USBAudioRecorder", "Step 1: Disabling streaming interface (alt 0)")
        val disableSuccess = setInterfaceNative(3, 0)
        if (!disableSuccess) {
            android.util.Log.e("USBAudioRecorder", "Failed to set interface to alt 0")
        }
        Thread.sleep(50)  // Brief delay for interface to settle

        android.util.Log.i("USBAudioRecorder", "Step 2: Requesting sample rate change to $rate Hz")
        val rateSetSuccess = setTargetSampleRateNative(rate)
        android.util.Log.i("USBAudioRecorder", "setTargetSampleRateNative($rate) returned: $rateSetSuccess")
        
        Thread.sleep(100)  // Give device time to reconfigure clock

        // Verify the change took effect
        val rateAfterSet = getEffectiveSampleRateNative()
        android.util.Log.i("USBAudioRecorder", "Device reports sample rate AFTER setTargetSampleRate: $rateAfterSet Hz")

        android.util.Log.i("USBAudioRecorder", "Step 3: Re-enabling streaming interface (alt 1)")
        val enableSuccess = setInterfaceNative(3, 1)
        if (!enableSuccess) {
            android.util.Log.e("USBAudioRecorder", "Failed to set interface to alt 1")
        }
        Thread.sleep(50)  // Brief delay for interface to settle

        // Final verification
        val rateFinal = getEffectiveSampleRateNative()
        android.util.Log.i("USBAudioRecorder", "Device reports sample rate FINAL: $rateFinal Hz")

        // Check if rate change was successful
        val success = (rateFinal == rate)
        if (success) {
            android.util.Log.i("USBAudioRecorder", "✅ Sample rate change SUCCESSFUL: $rateBefore Hz -> $rateFinal Hz")
            viewModel.setSelectedSampleRate(rate)
            viewModel.setNegotiatedSampleRate(rateFinal)
            refreshSampleRateCapabilities(rate, syncUiToDevice = false)
        } else {
            android.util.Log.w("USBAudioRecorder", "⚠️ Sample rate mismatch: wanted $rate Hz, got $rateFinal Hz")
            android.util.Log.w("USBAudioRecorder", "   setTargetSampleRateNative returned: $rateSetSuccess")
            android.util.Log.w("USBAudioRecorder", "   Rate progression: $rateBefore -> $rateAfterSet -> $rateFinal")
            // Still update UI to show what actually happened
            viewModel.setSelectedSampleRate(rate)  // Show user's intent
            viewModel.setNegotiatedSampleRate(rateFinal)  // Show device reality
            refreshSampleRateCapabilities(rate, syncUiToDevice = false)
        }

        android.util.Log.i("USBAudioRecorder", "=== onSampleRateSelected COMPLETE ===")
        return success
    }
    
    fun release() {
        android.util.Log.i("USBAudioRecorder", "Releasing USB audio recorder")
        
        stopRecording()
        levelUpdateJob?.cancel()
        
        // Release native resources
        try {
            releaseNativeAudio()
        } catch (e: UnsatisfiedLinkError) {
            // Native library not loaded or method not available - that's OK
        }
        isNativeInitialized = false
        viewModel.clearClipping()

        activeRecordingPfd?.closeSafely()
        activeRecordingPfd = null
        
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

private fun ParcelFileDescriptor.closeSafely() {
    try {
        close()
    } catch (_: IOException) {
        // Ignored
    }
}