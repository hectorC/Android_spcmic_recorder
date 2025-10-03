package com.spcmic.recorder

import android.Manifest
import android.content.Intent
import android.content.res.ColorStateList
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Bundle
import android.util.Log
import android.view.View
import android.view.animation.AnimationUtils
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.ViewCompat
import androidx.lifecycle.ViewModelProvider
import android.view.WindowManager
import com.google.android.material.card.MaterialCardView
import com.spcmic.recorder.databinding.ActivityMainBinding
import java.util.Locale

class MainActivity : AppCompatActivity() {
    
    private lateinit var binding: ActivityMainBinding
    private lateinit var viewModel: MainViewModel
    private lateinit var usbManager: UsbManager
    private lateinit var audioRecorder: USBAudioRecorder
    private lateinit var sampleRateAdapter: ArrayAdapter<String>
    private var suppressSampleRateCallback = false
    private var currentSupportedSampleRates: List<Int> = emptyList()
    private var lastSuccessfulSampleRate = 48000
    private var recordingPulseAnimation: android.view.animation.Animation? = null
    private var clipWarningAnimation: android.view.animation.Animation? = null
    
    private val requestPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { permissions ->
        val allGranted = permissions.all { it.value }
        if (allGranted) {
            initializeAudioRecorder()
        } else {
            Toast.makeText(this, "Audio recording permission is required", Toast.LENGTH_LONG).show()
        }
    }
    
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        
        viewModel = ViewModelProvider(this)[MainViewModel::class.java]
        usbManager = getSystemService(USB_SERVICE) as UsbManager
    lastSuccessfulSampleRate = viewModel.selectedSampleRate.value ?: 48000
        
        setupUI()
        checkPermissions()
        observeViewModel()
        
        // Handle USB device attachment if app was launched by USB intent
        handleUsbIntent(intent)
    }
    
    override fun onNewIntent(intent: Intent?) {
        super.onNewIntent(intent)
        intent?.let { handleUsbIntent(it) }
    }
    
    private fun handleUsbIntent(intent: Intent) {
        if (UsbManager.ACTION_USB_DEVICE_ATTACHED == intent.action) {
            val usbDevice: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            usbDevice?.let { device ->
                Log.i("MainActivity", "USB device attached: ${device.deviceName} (${device.manufacturerName} ${device.productName})")
                
                // Check if this is our target spcmic device
                if (device.vendorId == 22564 && device.productId == 10208) {
                    Log.i("MainActivity", "spcmic device detected - claiming device immediately")
                    
                    // Claim the device immediately to prevent Android audio system from taking it
                    claimUsbDevice(device)
                } else {
                    Log.i("MainActivity", "Generic USB audio device detected")
                    // Handle other USB audio devices if needed
                    refreshUSBDevices()
                }
            }
        }
    }
    
    private fun claimUsbDevice(device: UsbDevice) {
        try {
            // Initialize audio recorder if not already done
            if (!::audioRecorder.isInitialized) {
                audioRecorder = USBAudioRecorder(this, viewModel)
            }
            
            // Connect to the device immediately
            val success = audioRecorder.connectToDevice(device)
            if (success) {
                viewModel.setUSBDevice(device)
                Toast.makeText(this, "spcmic claimed successfully!", Toast.LENGTH_SHORT).show()
                Log.i("MainActivity", "Successfully claimed spcmic device")
            } else {
                Log.e("MainActivity", "Failed to claim spcmic device")
                Toast.makeText(this, "Failed to claim spcmic device", Toast.LENGTH_SHORT).show()
            }
        } catch (e: Exception) {
            Log.e("MainActivity", "Exception while claiming USB device", e)
            Toast.makeText(this, "Error claiming USB device: ${e.message}", Toast.LENGTH_LONG).show()
        }
    }
    
    private fun setupUI() {
        binding.apply {
            btnRecord.setOnClickListener {
                if (viewModel.isRecording.value == true) {
                    stopRecording()
                } else {
                    startRecording()
                }
            }
            
            btnRefreshDevices.setOnClickListener {
                refreshUSBDevices()
            }

            sampleRateAdapter = ArrayAdapter(
                this@MainActivity,
                android.R.layout.simple_spinner_item,
                mutableListOf()
            )
            sampleRateAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
            spinnerSampleRate.adapter = sampleRateAdapter
            spinnerSampleRate.isEnabled = false

            spinnerSampleRate.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>, view: View?, position: Int, id: Long) {
                    if (suppressSampleRateCallback) {
                        return
                    }

                    if (position < 0 || position >= currentSupportedSampleRates.size) {
                        return
                    }

                    val selectedRate = currentSupportedSampleRates[position]
                    val currentSelected = viewModel.selectedSampleRate.value
                    if (currentSelected == selectedRate) {
                        return
                    }

                    val recorderReady = ::audioRecorder.isInitialized && viewModel.isUSBDeviceConnected.value == true
                    if (recorderReady) {
                        val applied = audioRecorder.onSampleRateSelected(selectedRate)
                        if (applied) {
                            lastSuccessfulSampleRate = selectedRate
                        } else {
                            Toast.makeText(this@MainActivity, "Sample rate not accepted by device", Toast.LENGTH_SHORT).show()
                            revertSampleRateSelection()
                        }
                    } else {
                        viewModel.setSelectedSampleRate(selectedRate)
                        lastSuccessfulSampleRate = selectedRate
                    }
                }

                override fun onNothingSelected(parent: AdapterView<*>?) {
                    // No-op
                }
            }

            btnResetClip.setOnClickListener {
                if (::audioRecorder.isInitialized) {
                    audioRecorder.resetClipIndicator()
                } else {
                    viewModel.clearClipping()
                }
            }

            updateClipIndicator(viewModel.isClipping.value ?: false)
        }
    }
    
    private fun observeViewModel() {
        viewModel.isRecording.observe(this) { isRecording ->
            binding.btnRecord.text = if (isRecording) getString(R.string.record_button_stop) else getString(R.string.record_button_start)
            binding.btnRecord.isEnabled = viewModel.isUSBDeviceConnected.value == true
            
            // Update timer color and card appearance
            val colorRes = if (isRecording) R.color.timecode_recording else R.color.brand_on_surface
            binding.tvRecordingTime.setTextColor(ContextCompat.getColor(this, colorRes))
            
            // Update button icon
            val iconRes = if (isRecording) R.drawable.ic_stop else R.drawable.ic_microphone
            binding.btnRecord.setIconResource(iconRes)
            
            // Apply recording animations
            if (isRecording) {
                startRecordingAnimations()
            } else {
                stopRecordingAnimations()
            }
        }
        
        viewModel.isUSBDeviceConnected.observe(this) { isConnected ->
            binding.tvConnectionStatus.text = if (isConnected) {
                getString(R.string.usb_device_connected)
            } else {
                getString(R.string.usb_device_not_found)
            }
            binding.btnRecord.isEnabled = isConnected && !viewModel.isRecording.value!!
            binding.spinnerSampleRate.isEnabled = isConnected && currentSupportedSampleRates.isNotEmpty()
            
            // Update connection status badge
            val badgeRes = if (isConnected) R.drawable.status_badge_connected else R.drawable.status_badge_disconnected
            binding.connectionStatusBadge.setBackgroundResource(badgeRes)
        }
        
        viewModel.recordingTime.observe(this) { time ->
            binding.tvRecordingTime.text = formatTime(time)
        }
        
        viewModel.recordingFileName.observe(this) { fileName ->
            binding.tvRecordingFilename.text = if (fileName.isNullOrBlank()) " " else fileName
        }

        viewModel.channelLevels.observe(this) { levels ->
            binding.levelMeterView.updateLevels(levels)
        }

        viewModel.isClipping.observe(this) { isClipping ->
            updateClipIndicator(isClipping)
            
            // Start pulsing animation for clip warning
            if (isClipping) {
                startClipWarningAnimation()
            } else {
                stopClipWarningAnimation()
            }
        }

        viewModel.supportedSampleRates.observe(this) { rates ->
            currentSupportedSampleRates = rates
            if (::sampleRateAdapter.isInitialized) {
                val formatted = rates.map { formatSampleRate(it) }
                sampleRateAdapter.clear()
                if (formatted.isNotEmpty()) {
                    sampleRateAdapter.addAll(formatted)
                }
                sampleRateAdapter.notifyDataSetChanged()
            }
            binding.spinnerSampleRate.isEnabled = viewModel.isUSBDeviceConnected.value == true && rates.isNotEmpty()
            updateSampleRateSpinnerSelection()
            updateSampleRateSupportText()
        }

        viewModel.selectedSampleRate.observe(this) { rate ->
            binding.tvSampleRateStatus.text = "Selected sample rate: ${formatSampleRate(rate)}"
            binding.tvSampleRateInfo.text = "• Sample Rate: ${formatSampleRate(rate)}"
            updateSampleRateSpinnerSelection()
        }

        viewModel.negotiatedSampleRate.observe(this) { rate ->
            binding.tvNegotiatedSampleRate.text = "Device reported rate: ${formatSampleRate(rate)}"
            if (rate > 0) {
                lastSuccessfulSampleRate = rate
            }
        }

        viewModel.supportsContinuousSampleRate.observe(this) {
            updateSampleRateSupportText()
        }

        viewModel.continuousSampleRateRange.observe(this) {
            updateSampleRateSupportText()
        }
    }
    
    private fun checkPermissions() {
        val permissionsNeeded = mutableListOf<String>()
        
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) 
            != PackageManager.PERMISSION_GRANTED) {
            permissionsNeeded.add(Manifest.permission.RECORD_AUDIO)
        }
        
        if (permissionsNeeded.isNotEmpty()) {
            requestPermissionLauncher.launch(permissionsNeeded.toTypedArray())
        } else {
            initializeAudioRecorder()
        }
    }
    
    private fun initializeAudioRecorder() {
        audioRecorder = USBAudioRecorder(this, viewModel)
        // Don't auto-connect - wait for explicit USB device attachment or user action
        updateUSBDeviceList()
    }
    
    private fun updateUSBDeviceList() {
        // Just update the list without connecting
        val deviceList = usbManager.deviceList
        val audioDevices = deviceList.values.filter { device ->
            isAudioDevice(device)
        }
        
        if (audioDevices.isNotEmpty()) {
            val device = audioDevices.first()
            viewModel.setUSBDevice(device)
            // Don't auto-connect - wait for user action or USB intent
        } else {
            viewModel.setUSBDevice(null)
        }
    }
    
    private fun refreshUSBDevices() {
        // Refresh and connect to USB device (called by refresh button)
        val deviceList = usbManager.deviceList
        val audioDevices = deviceList.values.filter { device ->
            isAudioDevice(device)
        }
        
        if (audioDevices.isNotEmpty()) {
            val device = audioDevices.first()
            
            // Check if this is spcmic - use dedicated claiming
            if (device.vendorId == 22564 && device.productId == 10208) {
                claimUsbDevice(device)
            } else {
                viewModel.setUSBDevice(device)
                audioRecorder.connectToDevice(device)
            }
        } else {
            viewModel.setUSBDevice(null)
        }
    }
    
    private fun isAudioDevice(device: UsbDevice): Boolean {
        // Check if device is an audio interface
        // USB Audio Class: Class = 1 (Audio), Subclass = 1 (Audio Control) or 2 (Audio Streaming)
        for (i in 0 until device.interfaceCount) {
            val usbInterface = device.getInterface(i)
            if (usbInterface.interfaceClass == 1) { // USB_CLASS_AUDIO
                return true
            }
        }
        return false
    }
    
    private fun startRecording() {
        if (audioRecorder.startRecording()) {
            viewModel.startRecording()
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            Toast.makeText(this, "Failed to start recording", Toast.LENGTH_SHORT).show()
        }
    }
    
    private fun stopRecording() {
        audioRecorder.stopRecording()
        viewModel.stopRecording()
        window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    private fun updateSampleRateSpinnerSelection() {
        if (!::sampleRateAdapter.isInitialized) {
            return
        }

        val selectedRate = viewModel.selectedSampleRate.value ?: return
        val index = currentSupportedSampleRates.indexOf(selectedRate)

        if (index >= 0 && binding.spinnerSampleRate.selectedItemPosition != index) {
            suppressSampleRateCallback = true
            binding.spinnerSampleRate.setSelection(index, false)
            binding.spinnerSampleRate.post { suppressSampleRateCallback = false }
        }
    }

    private fun revertSampleRateSelection() {
        viewModel.setSelectedSampleRate(lastSuccessfulSampleRate)
        updateSampleRateSpinnerSelection()
    }

    private fun updateSampleRateSupportText() {
        val supportsContinuous = viewModel.supportsContinuousSampleRate.value == true
        val text = if (supportsContinuous) {
            val range = viewModel.continuousSampleRateRange.value
            if (range != null && range.first > 0 && range.second > 0) {
                "Device supports continuous range ${formatSampleRate(range.first)} – ${formatSampleRate(range.second)}"
            } else {
                "Device supports continuous sample rate selection"
            }
        } else {
            if (currentSupportedSampleRates.isEmpty()) {
                "Device sample rate capabilities unknown"
            } else {
                "Device supports discrete sample rates"
            }
        }
        binding.tvSampleRateSupport.text = text
    }

    private fun updateClipIndicator(isClipping: Boolean) {
        val indicator = binding.tvClipIndicator
        val icon = binding.ivClipIcon
        val colorRes = if (isClipping) R.color.clip_indicator_alert else R.color.clip_indicator_idle
        val iconRes = if (isClipping) R.drawable.ic_warning else R.drawable.ic_check_circle
        
        icon.setImageResource(iconRes)
        icon.imageTintList = ColorStateList.valueOf(ContextCompat.getColor(this, colorRes))
        indicator.text = if (isClipping) getString(R.string.clipping_detected) else getString(R.string.no_clipping_detected)
    }

    private fun formatSampleRate(rate: Int?): String {
        val value = rate ?: return "Unknown"
        if (value <= 0) return "Unknown"
        
        // Convert to kHz for cleaner display
        val kHz = value / 1000.0
        
        // Format with appropriate decimal places
        return if (kHz == kHz.toInt().toDouble()) {
            // Whole number (e.g., 48.0 -> "48 kHz")
            String.format(Locale.getDefault(), "%d kHz", kHz.toInt())
        } else {
            // Has decimals (e.g., 44.1 -> "44.1 kHz")
            String.format(Locale.getDefault(), "%.1f kHz", kHz)
        }
    }
    
    private fun formatTime(seconds: Long): String {
        val hours = seconds / 3600
        val minutes = (seconds % 3600) / 60
        val secs = seconds % 60
        return String.format("%02d:%02d:%02d", hours, minutes, secs)
    }
    
    private fun startRecordingAnimations() {
        // Pulse the record button
        if (recordingPulseAnimation == null) {
            recordingPulseAnimation = AnimationUtils.loadAnimation(this, R.anim.pulse_recording)
        }
        binding.btnRecord.startAnimation(recordingPulseAnimation)
        
        // Update timer card to recording style
        val timerCard = binding.timerCard
        timerCard.setCardBackgroundColor(ContextCompat.getColor(this, R.color.card_surface))
        timerCard.cardElevation = resources.getDimension(R.dimen.card_elevation_recording)
        timerCard.strokeWidth = 0
    }
    
    private fun stopRecordingAnimations() {
        // Stop button pulse
        binding.btnRecord.clearAnimation()
        
        // Reset timer card to idle style
        val timerCard = binding.timerCard
        timerCard.setCardBackgroundColor(ContextCompat.getColor(this, R.color.card_surface))
        timerCard.cardElevation = resources.getDimension(R.dimen.card_elevation)
        timerCard.strokeWidth = resources.getDimensionPixelSize(R.dimen.spacing_xs) / 4
    }
    
    private fun startClipWarningAnimation() {
        if (clipWarningAnimation == null) {
            clipWarningAnimation = AnimationUtils.loadAnimation(this, R.anim.pulse_clip_warning)
        }
        binding.ivClipIcon.startAnimation(clipWarningAnimation)
    }
    
    private fun stopClipWarningAnimation() {
        binding.ivClipIcon.clearAnimation()
    }
    
    override fun onDestroy() {
        super.onDestroy()
        if (::audioRecorder.isInitialized) {
            audioRecorder.release()
        }
    }
}